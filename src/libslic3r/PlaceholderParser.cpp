#include "PlaceholderParser.hpp"
#include "Exception.hpp"
#include "Flow.hpp"
#include "Utils.hpp"
#include <cstring>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <map>
#ifdef _MSC_VER
    #include <stdlib.h>  // provides **_environ
#else
    #include <unistd.h>  // provides **environ
#endif

#ifdef __APPLE__
#include <crt_externs.h>
#undef environ
#define environ (*_NSGetEnviron())
#else
    #ifdef _MSC_VER
       #define environ _environ
    #else
     	extern char **environ;
    #endif
#endif

#include <boost/algorithm/string.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/cstdlib.hpp>

// Spirit v2.5 allows you to suppress automatic generation
// of predefined terminals to speed up complation. With
// BOOST_SPIRIT_NO_PREDEFINED_TERMINALS defined, you are
// responsible in creating instances of the terminals that
// you need (e.g. see qi::uint_type uint_ below).
//#define BOOST_SPIRIT_NO_PREDEFINED_TERMINALS

#define BOOST_RESULT_OF_USE_DECLTYPE
#define BOOST_SPIRIT_USE_PHOENIX_V3
#include <boost/config/warning_disable.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/spirit/include/qi.hpp>
#include <boost/spirit/include/qi_lit.hpp>
#include <boost/phoenix/core.hpp>
#include <boost/phoenix/operator.hpp>
#include <boost/phoenix/fusion.hpp>
#include <boost/phoenix/stl.hpp>
#include <boost/phoenix/object.hpp>
#include <boost/fusion/include/adapt_struct.hpp>
#include <boost/spirit/repository/include/qi_distinct.hpp>
#include <boost/spirit/repository/include/qi_iter_pos.hpp>
#include <boost/variant/recursive_variant.hpp>
#include <boost/phoenix/bind/bind_function.hpp>

#include <iostream>
#include <string>

// #define USE_CPP11_REGEX
#ifdef USE_CPP11_REGEX
    #include <regex>
    #define SLIC3R_REGEX_NAMESPACE std
#else /* USE_CPP11_REGEX */
    #include <boost/regex.hpp>
    #define SLIC3R_REGEX_NAMESPACE boost
#endif /* USE_CPP11_REGEX */

namespace Slic3r {

PlaceholderParser::PlaceholderParser(const DynamicConfig *external_config) : m_external_config(external_config)
{
    this->set("version", std::string(SoftFever_VERSION));
    this->apply_env_variables();
    this->update_timestamp();
    this->update_user_name();
}

void PlaceholderParser::update_timestamp(DynamicConfig &config)
{
    time_t rawtime;
    time(&rawtime);
    struct tm* timeinfo = localtime(&rawtime);

    {
        std::ostringstream ss;
        ss << (1900 + timeinfo->tm_year);
        ss << std::setw(2) << std::setfill('0') << (1 + timeinfo->tm_mon);
        ss << std::setw(2) << std::setfill('0') << timeinfo->tm_mday;
        ss << "-";
        ss << std::setw(2) << std::setfill('0') << timeinfo->tm_hour;
        ss << std::setw(2) << std::setfill('0') << timeinfo->tm_min;
        ss << std::setw(2) << std::setfill('0') << timeinfo->tm_sec;
        config.set_key_value("timestamp", new ConfigOptionString(ss.str()));
    }
    config.set_key_value("year",   new ConfigOptionInt(1900 + timeinfo->tm_year));
    config.set_key_value("month",  new ConfigOptionInt(1 + timeinfo->tm_mon));
    config.set_key_value("day",    new ConfigOptionInt(timeinfo->tm_mday));
    config.set_key_value("hour",   new ConfigOptionInt(timeinfo->tm_hour));
    config.set_key_value("minute", new ConfigOptionInt(timeinfo->tm_min));
    config.set_key_value("second", new ConfigOptionInt(timeinfo->tm_sec));
}

void PlaceholderParser::update_user_name(DynamicConfig &config)
{
    const char* user = boost::nowide::getenv("USER") ? boost::nowide::getenv("USER") : boost::nowide::getenv("USERNAME") ? boost::nowide::getenv("USERNAME") : "unknown";
    config.set_key_value("user", new ConfigOptionString(user));
}

static inline bool opts_equal(const DynamicConfig &config_old, const DynamicConfig &config_new, const std::string &opt_key)
{
	const ConfigOption *opt_old = config_old.option(opt_key);
	const ConfigOption *opt_new = config_new.option(opt_key);
	assert(opt_new != nullptr);
    return opt_old != nullptr && *opt_new == *opt_old;
}

std::vector<std::string> PlaceholderParser::config_diff(const DynamicPrintConfig &rhs)
{
    std::vector<std::string> diff_keys;
    for (const t_config_option_key &opt_key : rhs.keys())
        if (! opts_equal(m_config, rhs, opt_key))
            diff_keys.emplace_back(opt_key);
    return diff_keys;
}

// Scalar configuration values are stored into m_single,
// vector configuration values are stored into m_multiple.
// All vector configuration values stored into the PlaceholderParser
// are expected to be addressed by the extruder ID, therefore
// if a vector configuration value is addressed without an index,
// a current extruder ID is used.
bool PlaceholderParser::apply_config(const DynamicPrintConfig &rhs)
{
    bool modified = false;
    for (const t_config_option_key &opt_key : rhs.keys()) {
        if (! opts_equal(m_config, rhs, opt_key)) {
			this->set(opt_key, rhs.option(opt_key)->clone());
            modified = true;
        }
    }
    return modified;
}

void PlaceholderParser::apply_only(const DynamicPrintConfig &rhs, const std::vector<std::string> &keys)
{
    for (const t_config_option_key &opt_key : keys)
        this->set(opt_key, rhs.option(opt_key)->clone());
}

void PlaceholderParser::apply_config(DynamicPrintConfig &&rhs)
{
	m_config += std::move(rhs);
}

void PlaceholderParser::apply_env_variables()
{
    for (char** env = environ; *env; ++ env) {
        if (strncmp(*env, "SLIC3R_", 7) == 0) {
            std::stringstream ss(*env);
            std::string key, value;
            std::getline(ss, key, '=');
            ss >> value;
            this->set(key, value);
        }
    }
}

namespace spirit = boost::spirit;
// Using an encoding, which accepts unsigned chars.
// Don't use boost::spirit::ascii, as it crashes internally due to indexing with negative char values for UTF8 characters into some 7bit character classification tables.
//namespace spirit_encoding = boost::spirit::ascii;
//FIXME iso8859_1 is just a workaround for the problem above. Replace it with UTF8 support!
namespace spirit_encoding = boost::spirit::iso8859_1;
namespace qi = boost::spirit::qi;
namespace px = boost::phoenix;

namespace client
{
    using Iterator      = std::string::const_iterator;
    using IteratorRange = boost::iterator_range<Iterator>;

    struct OptWithPos {
        OptWithPos() {}
        OptWithPos(ConfigOptionConstPtr opt, IteratorRange it_range, bool writable = false) : opt(opt), it_range(it_range), writable(writable) {}
        ConfigOptionConstPtr             opt { nullptr };
        bool                             writable { false };
        // -1 means it is a scalar variable, or it is a vector variable and index was not assigned yet or the whole vector is considered.
        int                              index { -1 };
        IteratorRange                    it_range;

        bool                             empty() const { return opt == nullptr; }
        bool                             has_index() const { return index != -1; }
    };

    std::ostream& operator<<(std::ostream& os, OptWithPos const& opt)
    {
        os << std::string(opt.it_range.begin(), opt.it_range.end());
        return os;
    }

    struct expr
    {
                 expr() {}
                 expr(const expr &rhs) : m_type(rhs.type()), it_range(rhs.it_range)
                    { if (rhs.type() == TYPE_STRING) m_data.s = new std::string(*rhs.m_data.s); else m_data.set(rhs.m_data); }
                 expr(expr &&rhs) : expr(std::move(rhs), rhs.it_range.begin(), rhs.it_range.end()) {}

        explicit expr(bool b) : m_type(TYPE_BOOL) { m_data.b = b; }
        explicit expr(bool b, const Iterator &it_begin, const Iterator &it_end) : m_type(TYPE_BOOL), it_range(it_begin, it_end) { m_data.b = b; }
        explicit expr(int i) : m_type(TYPE_INT) { m_data.i = i; }
        explicit expr(int i, const Iterator &it_begin, const Iterator &it_end) : m_type(TYPE_INT), it_range(it_begin, it_end) { m_data.i = i; }
        explicit expr(double d) : m_type(TYPE_DOUBLE) { m_data.d = d; }
        explicit expr(double d, const Iterator &it_begin, const Iterator &it_end) : m_type(TYPE_DOUBLE), it_range(it_begin, it_end) { m_data.d = d; }
        explicit expr(const char *s) : m_type(TYPE_STRING) { m_data.s = new std::string(s); }
        explicit expr(const std::string &s) : m_type(TYPE_STRING) { m_data.s = new std::string(s); }
        explicit expr(std::string &&s) : m_type(TYPE_STRING) { m_data.s = new std::string(std::move(s)); }
        explicit expr(const std::string &s, const Iterator &it_begin, const Iterator &it_end) :
            m_type(TYPE_STRING), it_range(it_begin, it_end) { m_data.s = new std::string(s); }
        explicit expr(expr &&rhs, const Iterator &it_begin, const Iterator &it_end) : m_type(rhs.type()), it_range{ it_begin, it_end }
        {
            m_data.set(rhs.m_data);
            rhs.m_type = TYPE_EMPTY;
        }
        expr &operator=(const expr &rhs)
        { 
            if (rhs.type() == TYPE_STRING) {
                this->set_s(rhs.s());
            } else  {
                m_type = rhs.type();
                m_data.set(rhs.m_data);
            }
            this->it_range = rhs.it_range;
            return *this;
        }

        expr &operator=(expr &&rhs)
        {
            if (this != &rhs) {
                this->reset();
                m_type          = rhs.type();
                this->it_range  = rhs.it_range;
                m_data.set(rhs.m_data);
                rhs.m_type      = TYPE_EMPTY;
            }
            return *this;
        }

        void                reset()
        { 
            if (this->type() == TYPE_STRING)
                delete m_data.s;
            m_type = TYPE_EMPTY;
        }
        ~expr() { reset(); }

        enum Type {
            TYPE_EMPTY = 0,
            TYPE_BOOL,
            TYPE_INT,
            TYPE_DOUBLE,
            TYPE_STRING,
        };
        Type                type() const { return m_type; }
        bool                numeric_type() const { return m_type == TYPE_INT || m_type == TYPE_DOUBLE; }

        bool&               b()       { return m_data.b; }
        bool                b() const { return m_data.b; }
        void                set_b(bool v) { this->reset(); this->set_b_lite(v); }
        void                set_b_lite(bool v) { assert(this->type() != TYPE_STRING); Data tmp; tmp.b = v; m_data.set(tmp); m_type = TYPE_BOOL; }
        int&                i()       { return m_data.i; }
        int                 i() const { return m_data.i; }
        void                set_i(int v) { this->reset(); set_i_lite(v); }
        void                set_i_lite(int v) { assert(this->type() != TYPE_STRING); Data tmp; tmp.i = v; m_data.set(tmp); m_type = TYPE_INT; }
        int                 as_i() const { return this->type() == TYPE_INT ? this->i() : int(this->d()); }
        int                 as_i_rounded() const { return this->type() == TYPE_INT ? this->i() : int(std::round(this->d())); }
        double&             d()       { return m_data.d; }
        double              d() const { return m_data.d; }
        void                set_d(double v) { this->reset(); this->set_d_lite(v); }
        void                set_d_lite(double v) { assert(this->type() != TYPE_STRING); Data tmp; tmp.d = v; m_data.set(tmp); m_type = TYPE_DOUBLE; }
        double              as_d() const { return this->type() == TYPE_DOUBLE ? this->d() : double(this->i()); }
        std::string&        s()       { return *m_data.s; }
        const std::string&  s() const { return *m_data.s; }
        void                set_s(const std::string &s) {
            if (this->type() == TYPE_STRING)
                *m_data.s = s;
            else 
                this->set_s_take_ownership(new std::string(s));
        }
        void                set_s(std::string &&s) {
            if (this->type() == TYPE_STRING)
                *m_data.s = std::move(s);
            else
                this->set_s_take_ownership(new std::string(std::move(s)));
        }
        void                set_s(const char *s) {
            if (this->type() == TYPE_STRING)
                *m_data.s = s;
            else
                this->set_s_take_ownership(new std::string(s));
        }

        std::string         to_string() const
        {
            std::string out;
            switch (this->type()) {
            case TYPE_EMPTY:
                // Inside an if / else block to be skipped.
                break;
			case TYPE_BOOL:   out = this->b() ? "true" : "false"; break;
            case TYPE_INT:    out = std::to_string(this->i()); break;
            case TYPE_DOUBLE:
#if 0
                // The default converter produces trailing zeros after the decimal point.
				out = std::to_string(data.d);
#else
                // ostringstream default converter produces no trailing zeros after the decimal point.
                // It seems to be doing what the old boost::to_string() did.
				{
					std::ostringstream ss;
					ss << this->d();
					out = ss.str();
				}
#endif
				break;
            case TYPE_STRING: out = this->s(); break;
            default:          break;
            }
            return out;
        }

        // Range of input iterators covering this expression.
        // Used for throwing parse exceptions.
        IteratorRange  it_range;

        expr unary_minus(const Iterator start_pos) const
        {
            switch (this->type()) {
            case TYPE_EMPTY:
                // Inside an if / else block to be skipped.
                return expr();
            case TYPE_INT :
                return expr(- this->i(), start_pos, this->it_range.end());
            case TYPE_DOUBLE:
                return expr(- this->d(), start_pos, this->it_range.end()); 
            default:
                this->throw_exception("Cannot apply unary minus operator.");
            }
            assert(false);
            // Suppress compiler warnings.
            return expr();
        }

        expr unary_integer(const Iterator start_pos) const
        {
            switch (this->type()) {
            case TYPE_EMPTY:
                // Inside an if / else block to be skipped.
                return expr();
            case TYPE_INT:
                return expr(this->i(), start_pos, this->it_range.end());
            case TYPE_DOUBLE:
                return expr(static_cast<int>(this->d()), start_pos, this->it_range.end()); 
            default:
                this->throw_exception("Cannot convert to integer.");
            }
            assert(false);
            // Suppress compiler warnings.
            return expr();
        }

        expr round(const Iterator start_pos) const
        {
            switch (this->type()) {
            case TYPE_EMPTY:
                // Inside an if / else block to be skipped.
                return expr();
            case TYPE_INT:
                return expr(this->i(), start_pos, this->it_range.end());
            case TYPE_DOUBLE:
                return expr(static_cast<int>(std::round(this->d())), start_pos, this->it_range.end());
            default:
                this->throw_exception("Cannot round a non-numeric value.");
            }
            assert(false);
            // Suppress compiler warnings.
            return expr();
        }

        expr floor(const Iterator start_pos)const
        {
            switch (this->type()) {
            case TYPE_INT:
                return expr(this->i(), start_pos, this->it_range.end());
            case TYPE_DOUBLE:
                return expr(static_cast<int>(std::floor(this->d())), start_pos, this->it_range.end());
            default:
                this->throw_exception("Cannot floor a non-numeric value.");
            }
            assert(false);
            // Suppress compiler warnings.
            return expr();
        }

        expr ceil(const Iterator start_pos)const
        {
            switch (this->type()) {
            case TYPE_INT:
                return expr(this->i(), start_pos, this->it_range.end());
            case TYPE_DOUBLE:
                return expr(static_cast<int>(std::ceil(this->d())), start_pos, this->it_range.end());
            default:
                this->throw_exception("Cannot ceil a non-numeric value.");
            }
            assert(false);
            // Suppress compiler warnings.
            return expr();
        }

        expr unary_not(const Iterator start_pos) const
        {
            switch (this->type()) {
            case TYPE_EMPTY:
                // Inside an if / else block to be skipped.
                return expr();
            case TYPE_BOOL:
                return expr(! this->b(), start_pos, this->it_range.end());
            default:
                this->throw_exception("Cannot apply a not operator.");
            }
            assert(false);
            // Suppress compiler warnings.
            return expr();
        }

        expr &operator+=(const expr &rhs)
        {
            if (this->type() == TYPE_EMPTY) {
                // Inside an if / else block to be skipped.
            } else if (this->type() == TYPE_STRING) {
                // Convert the right hand side to string and append.
                *m_data.s += rhs.to_string();
            } else if (rhs.type() == TYPE_STRING) {
                // Conver the left hand side to string, append rhs.
                this->set_s(this->to_string() + rhs.s());
            } else {
                const char *err_msg = "Cannot add non-numeric types.";
                this->throw_if_not_numeric(err_msg);
                rhs.throw_if_not_numeric(err_msg);
                if (this->type() == TYPE_DOUBLE || rhs.type() == TYPE_DOUBLE)
                    this->set_d_lite(this->as_d() + rhs.as_d());
                else
                    m_data.i += rhs.i();
            }
            this->it_range = IteratorRange(this->it_range.begin(), rhs.it_range.end());
            return *this;
        }

        expr &operator-=(const expr &rhs)
        {
            if (this->type() == TYPE_EMPTY) {
                // Inside an if / else block to be skipped.
                this->reset();
            } else {
                const char *err_msg = "Cannot subtract non-numeric types.";
                this->throw_if_not_numeric(err_msg);
                rhs.throw_if_not_numeric(err_msg);
                if (this->type() == TYPE_DOUBLE || rhs.type() == TYPE_DOUBLE)
                    this->set_d_lite(this->as_d() - rhs.as_d());
                else
                    m_data.i -= rhs.i();
                this->it_range = IteratorRange(this->it_range.begin(), rhs.it_range.end());
            }
            return *this;
        }

        expr &operator*=(const expr &rhs)
        {
            if (this->type() == TYPE_EMPTY) {
                // Inside an if / else block to be skipped.
                this->reset();
            } else {
                const char *err_msg = "Cannot multiply with non-numeric type.";
                this->throw_if_not_numeric(err_msg);
                rhs.throw_if_not_numeric(err_msg);
                if (this->type() == TYPE_DOUBLE || rhs.type() == TYPE_DOUBLE)
                    this->set_d_lite(this->as_d() * rhs.as_d());
                else
                    m_data.i *= rhs.i();
                this->it_range = IteratorRange(this->it_range.begin(), rhs.it_range.end());
            }
            return *this;
        }

        expr &operator/=(const expr &rhs)
        {
            if (this->type() == TYPE_EMPTY) {
                // Inside an if / else block to be skipped.
                this->reset();
            } else {
                this->throw_if_not_numeric("Cannot divide a non-numeric type.");
                rhs.throw_if_not_numeric("Cannot divide with a non-numeric type.");
                if (rhs.type() == TYPE_INT ? (rhs.i() == 0) : (rhs.d() == 0.))
                    rhs.throw_exception("Division by zero");
                if (this->type() == TYPE_DOUBLE || rhs.type() == TYPE_DOUBLE)
                    this->set_d_lite(this->as_d() / rhs.as_d());
                else
                    m_data.i /= rhs.i();
                this->it_range = IteratorRange(this->it_range.begin(), rhs.it_range.end());
            }
            return *this;
        }

        expr &operator%=(const expr &rhs)
        {
            if (this->type() == TYPE_EMPTY) {
                // Inside an if / else block to be skipped.
                this->reset();
            } else {
                this->throw_if_not_numeric("Cannot divide a non-numeric type.");
                rhs.throw_if_not_numeric("Cannot divide with a non-numeric type.");
                if (rhs.type() == TYPE_INT ? (rhs.i() == 0) : (rhs.d() == 0.))
                    rhs.throw_exception("Division by zero");
                if (this->type() == TYPE_DOUBLE || rhs.type() == TYPE_DOUBLE)
                    this->set_d_lite(std::fmod(this->as_d(), rhs.as_d()));
                else
                    m_data.i %= rhs.i();
                this->it_range = IteratorRange(this->it_range.begin(), rhs.it_range.end());
            }
            return *this;
        }

        static void to_string2(expr &self, std::string &out)
        {
            if (self.type() != TYPE_EMPTY)
                // Not inside an if / else block to be skipped
                out = self.to_string();
        }

        static void evaluate_boolean(expr &self, bool &out)
        {
            if (self.type() != TYPE_EMPTY) {
                // Not inside an if / else block to be skipped
                if (self.type() != TYPE_BOOL)
                    self.throw_exception("Not a boolean expression");
                out = self.b();
            }
        }

        static void evaluate_boolean_to_string(expr &self, std::string &out)
        {
            assert(self.type() != TYPE_EMPTY);
            if (self.type() != TYPE_BOOL)
                self.throw_exception("Not a boolean expression");
            out = self.b() ? "true" : "false";
        }

        // Is lhs==rhs? Store the result into lhs.
        static void compare_op(expr &lhs, expr &rhs, char op, bool invert)
        {
            if (lhs.type() == TYPE_EMPTY)
                // Inside an if / else block to be skipped
                return;
            bool value = false;
            if (lhs.numeric_type() && rhs.numeric_type()) {
                // Both types are numeric.
                switch (op) {
                    case '=':
                        value = (lhs.type() == TYPE_DOUBLE || rhs.type() == TYPE_DOUBLE) ?
                            (std::abs(lhs.as_d() - rhs.as_d()) < 1e-8) : (lhs.i() == rhs.i());
                        break;
                    case '<':
                        value = (lhs.type() == TYPE_DOUBLE || rhs.type() == TYPE_DOUBLE) ?
                            (lhs.as_d() < rhs.as_d()) : (lhs.i() < rhs.i());
                        break;
                    case '>':
                    default:
                        value = (lhs.type() == TYPE_DOUBLE || rhs.type() == TYPE_DOUBLE) ?
                            (lhs.as_d() > rhs.as_d()) : (lhs.i() > rhs.i());
                        break;
                }
            } else if (lhs.type() == TYPE_BOOL && rhs.type() == TYPE_BOOL) {
                // Both type are bool.
                if (op != '=')
                    boost::throw_exception(qi::expectation_failure<Iterator>(
                        lhs.it_range.begin(), rhs.it_range.end(), spirit::info("*Cannot compare the types.")));
                value = lhs.b() == rhs.b();
            } else if (lhs.type() == TYPE_STRING || rhs.type() == TYPE_STRING) {
                // One type is string, the other could be converted to string.
                value = (op == '=') ? (lhs.to_string() == rhs.to_string()) :
                        (op == '<') ? (lhs.to_string() < rhs.to_string()) : (lhs.to_string() > rhs.to_string());
            } else {
                boost::throw_exception(qi::expectation_failure<Iterator>(
                    lhs.it_range.begin(), rhs.it_range.end(), spirit::info("*Cannot compare the types.")));
            }
            lhs.reset();
            lhs.set_b_lite(invert ? ! value : value);
        }
        // Compare operators, store the result into lhs.
        static void equal    (expr &lhs, expr &rhs) { compare_op(lhs, rhs, '=', false); }
        static void not_equal(expr &lhs, expr &rhs) { compare_op(lhs, rhs, '=', true ); }
        static void lower    (expr &lhs, expr &rhs) { compare_op(lhs, rhs, '<', false); }
        static void greater  (expr &lhs, expr &rhs) { compare_op(lhs, rhs, '>', false); }
        static void leq      (expr &lhs, expr &rhs) { compare_op(lhs, rhs, '>', true ); }
        static void geq      (expr &lhs, expr &rhs) { compare_op(lhs, rhs, '<', true ); }

        static void throw_if_not_numeric(const expr &param)
        {
            const char *err_msg = "Not a numeric type.";
            param.throw_if_not_numeric(err_msg);
        }

        enum Function2ParamsType {
            FUNCTION_MIN,
            FUNCTION_MAX,
        };
        // Store the result into param1.
        static void function_2params(expr &param1, expr &param2, Function2ParamsType fun)
        {
            if (param1.type() == TYPE_EMPTY)
                // Inside an if / else block to be skipped
                return;
            throw_if_not_numeric(param1);
            throw_if_not_numeric(param2);
            if (param1.type() == TYPE_DOUBLE || param2.type() == TYPE_DOUBLE) {
                double d = 0.;
                switch (fun) {
                    case FUNCTION_MIN:  d = std::min(param1.as_d(), param2.as_d()); break;
                    case FUNCTION_MAX:  d = std::max(param1.as_d(), param2.as_d()); break;
                    default: param1.throw_exception("Internal error: invalid function");
                }
                param1.set_d_lite(d);
            } else {
                int i = 0;
                switch (fun) {
                    case FUNCTION_MIN:  i = std::min(param1.as_i(), param2.as_i()); break;
                    case FUNCTION_MAX:  i = std::max(param1.as_i(), param2.as_i()); break;
                    default: param1.throw_exception("Internal error: invalid function");
                }
                param1.set_i_lite(i);
            }
        }
        // Store the result into param1.
        static void min(expr &param1, expr &param2) { function_2params(param1, param2, FUNCTION_MIN); }
        static void max(expr &param1, expr &param2) { function_2params(param1, param2, FUNCTION_MAX); }

        // Store the result into param1.
        static void random(expr &param1, expr &param2, std::mt19937 &rng)
        {
            if (param1.type() == TYPE_EMPTY)
                // Inside an if / else block to be skipped
                return;
            throw_if_not_numeric(param1);
            throw_if_not_numeric(param2);
            if (param1.type() == TYPE_DOUBLE || param2.type() == TYPE_DOUBLE)
                param1.set_d_lite(std::uniform_real_distribution<>(param1.as_d(), param2.as_d())(rng));
            else
                param1.set_i_lite(std::uniform_int_distribution<>(param1.as_i(), param2.as_i())(rng));
        }

        // Store the result into param1.
        // param3 is optional
        template<bool leading_zeros>
        static void digits(expr &param1, expr &param2, expr &param3)
        {
            if (param1.type() == TYPE_EMPTY)
                // Inside an if / else block to be skipped
                return;
            throw_if_not_numeric(param1);
            if (param2.type() != TYPE_INT)
                param2.throw_exception("digits: second parameter must be integer");
            bool has_decimals = param3.type() != TYPE_EMPTY;
            if (has_decimals && param3.type() != TYPE_INT)
                param3.throw_exception("digits: third parameter must be integer");

            char buf[256];
            int  ndigits = std::clamp(param2.as_i(), 0, 64);
            if (has_decimals) {
                // Format as double.
                int decimals = std::clamp(param3.as_i(), 0, 64);
                sprintf(buf, leading_zeros ? "%0*.*lf" : "%*.*lf", ndigits, decimals, param1.as_d());
            } else
                // Format as int.
                sprintf(buf, leading_zeros ? "%0*d" : "%*d", ndigits, param1.as_i_rounded());
            param1.set_s(buf);
        }

        static void regex_op(const expr &lhs, IteratorRange &rhs, char op, expr &out)
        {
            if (lhs.type() == TYPE_EMPTY)
                // Inside an if / else block to be skipped
                return;
            const std::string *subject  = nullptr;
            if (lhs.type() == TYPE_STRING) {
                // One type is string, the other could be converted to string.
                subject = &lhs.s();
            } else {
                lhs.throw_exception("Left hand side of a regex match must be a string.");
            }
            try {
                std::string pattern(++ rhs.begin(), -- rhs.end());
                bool result = SLIC3R_REGEX_NAMESPACE::regex_match(*subject, SLIC3R_REGEX_NAMESPACE::regex(pattern));
                if (op == '!')
                    result = ! result;
                out.set_b(result);
            } catch (SLIC3R_REGEX_NAMESPACE::regex_error &ex) {
                // Syntax error in the regular expression
                boost::throw_exception(qi::expectation_failure<Iterator>(
                    rhs.begin(), rhs.end(), spirit::info(std::string("*Regular expression compilation failed: ") + ex.what())));
            }
        }

        static void regex_matches     (expr &lhs, IteratorRange &rhs) { return regex_op(lhs, rhs, '=', lhs); }
        static void regex_doesnt_match(expr &lhs, IteratorRange &rhs) { return regex_op(lhs, rhs, '!', lhs); }

        static void one_of_test_init(expr &out) {
            out.set_b(false);
        }
        template<bool RegEx>
        static void one_of_test(const expr &match, const expr &pattern, expr &out) { 
            if (match.type() == TYPE_EMPTY) {
                // Inside an if / else block to be skipped
                out.reset();
                return;            
            }
            if (! out.b()) {
                if (match.type() != TYPE_STRING)
                    match.throw_exception("one_of(): First parameter (the string to match against) has to be a string value");
                if (pattern.type() != TYPE_STRING)
                    match.throw_exception("one_of(): Pattern has to be a string value");
                if (RegEx) {
                    try {
                        out.set_b(SLIC3R_REGEX_NAMESPACE::regex_match(match.s(), SLIC3R_REGEX_NAMESPACE::regex(pattern.s())));
                    } catch (SLIC3R_REGEX_NAMESPACE::regex_error &) {
                        // Syntax error in the regular expression
                        pattern.throw_exception("Regular expression compilation failed");
                    }
                } else
                    out.set_b(match.s() == pattern.s());
            }
        }
        static void one_of_test_regex(const expr &match, IteratorRange &pattern, expr &out) {
            if (match.type() == TYPE_EMPTY) {
                // Inside an if / else block to be skipped
                out.reset();
                return;            
            }
            if (! out.b()) {
                if (match.type() != TYPE_STRING)
                    match.throw_exception("one_of(): First parameter (the string to match against) has to be a string value");
                regex_op(match, pattern, '=', out);
            }
        }

        static void logical_op(expr &lhs, expr &rhs, char op)
        {
            if (lhs.type() == TYPE_EMPTY)
                // Inside an if / else block to be skipped
                return;            
            bool value = false;
            if (lhs.type() == TYPE_BOOL && rhs.type() == TYPE_BOOL) {
                value = (op == '|') ? (lhs.b() || rhs.b()) : (lhs.b() && rhs.b());
            } else {
                boost::throw_exception(qi::expectation_failure<Iterator>(
                    lhs.it_range.begin(), rhs.it_range.end(), spirit::info("*Cannot apply logical operation to non-boolean operators.")));
            }
            lhs.set_b_lite(value);
        }
        static void logical_or (expr &lhs, expr &rhs) { logical_op(lhs, rhs, '|'); }
        static void logical_and(expr &lhs, expr &rhs) { logical_op(lhs, rhs, '&'); }

        void throw_exception(const char *message) const
        {
            boost::throw_exception(qi::expectation_failure<Iterator>(
                this->it_range.begin(), this->it_range.end(), spirit::info(std::string("*") + message)));
        }

        void throw_if_not_numeric(const char *message) const
        {
            if (! this->numeric_type())
                this->throw_exception(message);
        }

    private:
        // This object will take ownership of the parameter string object "s".
        void        set_s_take_ownership(std::string* s) { assert(this->type() != TYPE_STRING); Data tmp; tmp.s = s; m_data.set(tmp); m_type = TYPE_STRING; }

        Type        m_type = TYPE_EMPTY;

        union Data {
            bool         b;
            int          i;
            double       d;
            std::string *s;

            // Copy the largest member variable through char*, which will alias with all other union members by default.
            void set(const Data &rhs) { memcpy(this, &rhs, sizeof(rhs)); }
        } m_data;
    };

    std::ostream& operator<<(std::ostream &os, const expr &expression)
    {
        typedef expr Expr;
        os << std::string(expression.it_range.begin(), expression.it_range.end()) << " - ";
        switch (expression.type()) {
        case Expr::TYPE_EMPTY:    os << "empty"; break;
        case Expr::TYPE_BOOL:     os << "bool ("   << expression.b() << ")"; break;
        case Expr::TYPE_INT:      os << "int ("    << expression.i() << ")"; break;
        case Expr::TYPE_DOUBLE:   os << "double (" << expression.d() << ")"; break;
        case Expr::TYPE_STRING:   os << "string (" << expression.s() << ")"; break;
        default: os << "unknown";
        };
        return os;
    }

    struct MyContext : public ConfigOptionResolver {
        // Config provided as a parameter to PlaceholderParser invocation, overriding PlaceholderParser stored config.
    	const DynamicConfig     *external_config        = nullptr;
        // Config stored inside PlaceholderParser.
        const DynamicConfig     *config                 = nullptr;
        // Config provided as a parameter to PlaceholderParser invocation, evaluated after the two configs above.
        const DynamicConfig     *config_override        = nullptr;
        // Config provided as a parameter to PlaceholderParser invocation, containing variables that will be read out 
        // and processed by the PlaceholderParser callee.
        mutable DynamicConfig   *config_outputs         = nullptr;
        // Local variables, read / write
        mutable DynamicConfig    config_local;
        size_t                   current_extruder_id    = 0;  // This is filament_id actually
        // Random number generator and optionally global variables.
        PlaceholderParser::ContextData *context_data    = nullptr;
        // If false, the macro_processor will evaluate a full macro.
        // If true, the macro processor will evaluate just a boolean condition using the full expressive power of the macro processor.
        bool                     just_boolean_expression = false;
        std::string              error_message;

        // Table to translate symbol tag to a human readable error message.
        static std::map<std::string, std::string> tag_to_error_message;

        size_t get_extruder_id() const {
            const ConfigOptionInts * filament_map_opt = external_config->option<ConfigOptionInts>("filament_map");
            if (filament_map_opt && current_extruder_id < filament_map_opt->values.size()) {
                return filament_map_opt->values[current_extruder_id];
            }
            return 0;
        }

        static bool             evaluate_full_macro(const MyContext *ctx) { return ! ctx->just_boolean_expression; }

        // Entering a conditional block.
        static void block_enter(const MyContext *ctx, const bool condition)
        {
            if (ctx->skipping() || ! condition)
                ++ ctx->m_depth_suppressed;
        }
        // Exiting a conditional block.
        static void block_exit(const MyContext *ctx, const bool condition, bool &not_yet_consumed, std::string &data_in, std::string &data_out)
        {
            if (ctx->skipping())
                -- ctx->m_depth_suppressed;
            else if (condition && not_yet_consumed) {
                data_out = std::move(data_in);
                not_yet_consumed = false;
            }
        }
        static void block_exit_ternary(const MyContext* ctx, const bool condition, expr &data_in, expr &data_out)
        {
            if (ctx->skipping())
                -- ctx->m_depth_suppressed;
            else if (condition)
                data_out = std::move(data_in);
        }
        // Inside a block, which is conditionally suppressed?
        bool skipping() const { return m_depth_suppressed > 0; }

        const ConfigOption* 	optptr(const t_config_option_key &opt_key) const override
        {
            const ConfigOption *opt = nullptr;
            if (config_override != nullptr)
                opt = config_override->option(opt_key);
            if (opt == nullptr)
                opt = config->option(opt_key);
            if (opt == nullptr && external_config != nullptr)
                opt = external_config->option(opt_key);
            return opt;
        }

        const ConfigOption*     resolve_symbol(const std::string &opt_key) const { return this->optptr(opt_key); }
        ConfigOption*           resolve_output_symbol(const std::string &opt_key) const {
            ConfigOption *out = nullptr;
            if (this->config_outputs)
                out = this->config_outputs->optptr(opt_key, false);
            if (out == nullptr && this->context_data != nullptr && this->context_data->global_config)
                out = this->context_data->global_config->optptr(opt_key);
            if (out == nullptr)
                out = this->config_local.optptr(opt_key);
            return out;
        }
        void                    store_new_variable(const std::string &opt_key, std::unique_ptr<ConfigOption> &&opt, bool global_variable) {
            assert(opt);
            if (global_variable) {
                assert(this->context_data != nullptr && this->context_data->global_config);
                this->context_data->global_config->set_key_value(opt_key, opt.release());
            } else
                this->config_local.set_key_value(opt_key, opt.release());
        }

        static void legacy_variable_expansion(const MyContext *ctx, IteratorRange &opt_key, std::string &output)
        {
            if (ctx->skipping())
                return;

            std::string         opt_key_str(opt_key.begin(), opt_key.end());
            const ConfigOption *opt = ctx->resolve_symbol(opt_key_str);
            size_t              idx = ctx->current_extruder_id;
            if (opt == nullptr) {
                // Check whether this is a legacy vector indexing.
                idx = opt_key_str.rfind('_');
                if (idx != std::string::npos) {
                    opt = ctx->resolve_symbol(opt_key_str.substr(0, idx));
                    if (opt != nullptr) {
                        if (! opt->is_vector())
                            ctx->throw_exception("Trying to index a scalar variable", opt_key);
                        char *endptr = nullptr;
                        idx = strtol(opt_key_str.c_str() + idx + 1, &endptr, 10);
                        if (endptr == nullptr || *endptr != 0)
                            ctx->throw_exception("Invalid vector index", IteratorRange(opt_key.begin() + idx + 1, opt_key.end()));
                    }
                }
            }
            if (opt == nullptr)
                ctx->throw_exception("Variable does not exist", opt_key);
            if (opt->is_scalar()) {
                if (opt->is_nil())
                    ctx->throw_exception("Trying to reference an undefined (nil) optional variable", opt_key);
                output = opt->serialize();
            } else {
                const ConfigOptionVectorBase *vec = static_cast<const ConfigOptionVectorBase*>(opt);
                if (vec->empty())
                    ctx->throw_exception("Indexing an empty vector variable", opt_key);
                if (idx >= vec->size())
                    idx = 0;
                if (vec->is_nil(idx))
                    ctx->throw_exception("Trying to reference an undefined (nil) element of vector of optional values", opt_key);
                output = vec->vserialize()[idx];
            }
        }

        static void legacy_variable_expansion2(
            const MyContext *ctx,
            IteratorRange   &opt_key,
            IteratorRange   &opt_vector_index,
            std::string     &output)
        {
            if (ctx->skipping())
                return;

            std::string         opt_key_str(opt_key.begin(), opt_key.end());
            const ConfigOption *opt = ctx->resolve_symbol(opt_key_str);
            if (opt == nullptr) {
                // Check whether the opt_key ends with '_'.
                if (opt_key_str.back() == '_') {
                    opt_key_str.resize(opt_key_str.size() - 1);
                    opt = ctx->resolve_symbol(opt_key_str);
                }
                if (opt == nullptr)
                    ctx->throw_exception("Variable does not exist", opt_key);
            }
            if (! opt->is_vector())
                ctx->throw_exception("Trying to index a scalar variable", opt_key);
            const ConfigOptionVectorBase *vec = static_cast<const ConfigOptionVectorBase*>(opt);
            if (vec->empty())
                ctx->throw_exception("Indexing an empty vector variable", opt_key);
            const ConfigOption *opt_index = ctx->resolve_symbol(std::string(opt_vector_index.begin(), opt_vector_index.end()));
            if (opt_index == nullptr)
                ctx->throw_exception("Variable does not exist", opt_key);
            if (opt_index->type() != coInt)
                ctx->throw_exception("Indexing variable has to be integer", opt_key);
			int idx = opt_index->getInt();
			if (idx < 0)
                ctx->throw_exception("Negative vector index", opt_key);
            if (idx >= (int)vec->size())
                idx = 0;
            if (vec->is_nil(idx))
                ctx->throw_exception("Trying to reference an undefined (nil) element of vector of optional values", opt_key);
			output = vec->vserialize()[idx];
        }

        static void resolve_variable(
            const MyContext     *ctx,
            IteratorRange       &opt_key,
            OptWithPos          &output)
        {
            if (! ctx->skipping()) {
                const std::string key{ opt_key.begin(), opt_key.end() };
                const ConfigOption *opt = ctx->resolve_symbol(key);
                if (opt == nullptr) {
                    opt = ctx->resolve_output_symbol(key);
                    if (opt == nullptr)
                        ctx->throw_exception("Not a variable name", opt_key);
                    output.writable = true;
                }
                output.opt = opt;
            }
            output.it_range = opt_key;
        }

        static void store_variable_index(
            const MyContext *ctx,
           OptWithPos       &opt,
           int               index,
           Iterator          it_end,
           OptWithPos       &output)
        {
            if (! ctx->skipping()) {
                if (index < 0)
                    index = 0; // Orca: fallback to first element if index < 0, this matches the behavior of BambuStudio
                if (!opt.opt->is_vector())
                    index = -1; // Orca: ignore the index if variable is scalar, this matches the behavior of BambuStudio
                output = opt;
                output.index = index;
            } else
                output = opt;
            output.it_range.end()   = it_end;
        }

        // Evaluating a scalar variable into expr,
        // all possible ConfigOption types are supported.
        static void scalar_variable_to_expr(const MyContext *ctx, OptWithPos &opt, expr &output)
        {
            if (ctx->skipping())
                return;

            assert(opt.opt->is_scalar());

            if (opt.opt->is_nil())
                ctx->throw_exception("Trying to reference an undefined (nil) optional variable", opt.it_range);

            switch (opt.opt->type()) {
            case coFloat:   output.set_d(opt.opt->getFloat());   break;
            case coInt:     output.set_i(opt.opt->getInt());     break;
            case coString:  output.set_s(static_cast<const ConfigOptionString*>(opt.opt)->value); break;
            case coPercent: output.set_d(opt.opt->getFloat());   break;
            case coEnum:
            case coPoint:   output.set_s(opt.opt->serialize());  break;
            case coBool:    output.set_b(opt.opt->getBool());    break;
            case coFloatOrPercent:
            {
                std::string opt_key(opt.it_range.begin(), opt.it_range.end());
                if (boost::ends_with(opt_key, "line_width")) {
                    // Line width supports defaults and a complex graph of dependencies.
                    output.set_d(Flow::extrusion_width(opt_key, *ctx, static_cast<unsigned int>(ctx->current_extruder_id)));
                } else if (! static_cast<const ConfigOptionFloatOrPercent*>(opt.opt)->percent) {
                    // Not a percent, just return the value.
                    output.set_d(opt.opt->getFloat());
                } else {
                    // Resolve dependencies using the "ratio_over" link to a parent value.
    			    const ConfigOptionDef  *opt_def = print_config_def.get(opt_key);
    			    assert(opt_def != nullptr);
    			    double v = opt.opt->getFloat() * 0.01; // percent to ratio
    			    for (;;) {
    			        const ConfigOption *opt_parent = opt_def->ratio_over.empty() ? nullptr : ctx->resolve_symbol(opt_def->ratio_over);
    			        if (opt_parent == nullptr)
    			            ctx->throw_exception("FloatOrPercent variable failed to resolve the \"ratio_over\" dependencies", opt.it_range);
    			        if (boost::ends_with(opt_def->ratio_over, "line_width")) {
                    		// Line width supports defaults and a complex graph of dependencies.
                            assert(opt_parent->type() == coFloatOrPercent);
                        	v *= Flow::extrusion_width(opt_def->ratio_over, static_cast<const ConfigOptionFloatOrPercent*>(opt_parent), *ctx, static_cast<unsigned int>(ctx->current_extruder_id));
                        	break;
                        }
                        if (opt_parent->type() == coFloat || opt_parent->type() == coFloatOrPercent) {
    			        	v *= opt_parent->getFloat();
    			        	if (opt_parent->type() == coFloat || ! static_cast<const ConfigOptionFloatOrPercent*>(opt_parent)->percent)
    			        		break;
    			        	v *= 0.01; // percent to ratio
    			        }
    		        	// Continue one level up in the "ratio_over" hierarchy.
    				    opt_def = print_config_def.get(opt_def->ratio_over);
    				    assert(opt_def != nullptr);
    			    }
                    output.set_d(v);
    	        }
    		    break;
    		}
            default:
                ctx->throw_exception("Unsupported scalar variable type", opt.it_range);
            }
        }

        // Evaluating one element of a vector variable.
        // all possible ConfigOption types are supported.
        static void vector_element_to_expr(const MyContext *ctx, OptWithPos &opt, expr &output)
        {
            if (ctx->skipping())
                return;

            assert(opt.opt->is_vector());
            const ConfigOptionVectorBase* vec = static_cast<const ConfigOptionVectorBase*>(opt.opt);
            if (vec->empty())
                ctx->throw_exception("Indexing an empty vector variable", opt.it_range);
            if (!opt.has_index()) {
                // Allow omitting extruder id when referencing vectors
                switch (opt.opt->type()) {
                case coFloats: {
                    const ConfigOptionFloatsNullable* opt_floatsnullable = static_cast<const ConfigOptionFloatsNullable *>(opt.opt);
                    if (opt_floatsnullable) {
                        if (opt_floatsnullable->size() == 1) { // old version
                            output.set_d(static_cast<const ConfigOptionFloatsNullable*>(opt.opt)->get_at(0));
                        } else {
                            output.set_d(static_cast<const ConfigOptionFloatsNullable*>(opt.opt)->get_at(ctx->get_extruder_id()));
                        }
                    } else {
                        const ConfigOptionFloats* opt_floats = static_cast<const ConfigOptionFloats*>(opt.opt);
                        if (opt_floats->size() == 1) { // old version
                            output.set_d(static_cast<const ConfigOptionFloats*>(opt.opt)->get_at(0));
                        } else {
                            output.set_d(static_cast<const ConfigOptionFloats*>(opt.opt)->get_at(ctx->get_extruder_id()));
                        }
                    }
                    break;
                }
                default: ctx->throw_exception("Referencing a vector variable when scalar is expected", opt.it_range);
                }
            } else {
                size_t idx = (opt.index < 0) ? 0 : (opt.index >= int(vec->size())) ? 0 : size_t(opt.index);
                if (vec->is_nil(idx))
                    ctx->throw_exception("Trying to reference an undefined (nil) element of vector of optional values", opt.it_range);
                switch (opt.opt->type()) {
                case coFloats:   output.set_d(static_cast<const ConfigOptionFloats*>(opt.opt)->values[idx]); break;
                case coInts:     output.set_i(static_cast<const ConfigOptionInts*>(opt.opt)->values[idx]); break;
                case coStrings:  output.set_s(static_cast<const ConfigOptionStrings*>(opt.opt)->values[idx]); break;
                case coPercents: output.set_d(static_cast<const ConfigOptionPercents*>(opt.opt)->values[idx]); break;
                case coPoints:   output.set_s(to_string(static_cast<const ConfigOptionPoints*>(opt.opt)->values[idx])); break;
                case coBools:    output.set_b(static_cast<const ConfigOptionBools*>(opt.opt)->values[idx] != 0); break;
                case coEnums:    output.set_i(static_cast<const ConfigOptionInts    *>(opt.opt)->values[idx]); break;
                default:
                    ctx->throw_exception("Unsupported vector variable type", opt.it_range);
                }
            }
        }

        static void check_writable(const MyContext *ctx, OptWithPos &opt) {
            if (! opt.writable)
                ctx->throw_exception("Cannot modify a read-only variable", opt.it_range);
        }

        static void check_numeric(const expr &param) {
            if (! param.numeric_type())
                param.throw_exception("Right side is not a numeric expression");
        };

        static size_t evaluate_count(const expr &expr_count) {
            if (expr_count.type() != expr::TYPE_INT)
                expr_count.throw_exception("Expected number of elements to fill a vector with.");
            int count = expr_count.i();
            if (count < 0)
                expr_count.throw_exception("Negative number of elements specified.");
            return size_t(count);
        };

        static void scalar_variable_assign_scalar(const MyContext *ctx, OptWithPos &lhs, const expr &rhs)
        {
            assert(! ctx->skipping());
            assert(lhs.opt->is_scalar());
            check_writable(ctx, lhs);
            ConfigOption *wropt = const_cast<ConfigOption*>(lhs.opt);
            switch (wropt->type()) {
            case coFloat:
                check_numeric(rhs);
                static_cast<ConfigOptionFloat*>(wropt)->value = rhs.as_d();
                break;
            case coInt:
                check_numeric(rhs);
                static_cast<ConfigOptionInt*>(wropt)->value = rhs.as_i();
                break;
            case coString:
                static_cast<ConfigOptionString*>(wropt)->value = rhs.to_string();
                break;
            case coPercent:
                check_numeric(rhs);
                static_cast<ConfigOptionPercent*>(wropt)->value = rhs.as_d();
                break;
            case coBool:
                if (rhs.type() != expr::TYPE_BOOL)
                    ctx->throw_exception("Right side is not a boolean expression", rhs.it_range);
                static_cast<ConfigOptionBool*>(wropt)->value = rhs.b();
                break;
            default:
                ctx->throw_exception("Unsupported output scalar variable type", lhs.it_range);
            }
        }

        static void vector_variable_element_assign_scalar(const MyContext *ctx, OptWithPos &lhs, const expr &rhs)
        {
            assert(! ctx->skipping());
            assert(lhs.opt->is_vector());
            check_writable(ctx, lhs);
            if (! lhs.has_index())
                ctx->throw_exception("Referencing an output vector variable when scalar is expected", lhs.it_range);
            ConfigOptionVectorBase *vec = const_cast<ConfigOptionVectorBase*>(static_cast<const ConfigOptionVectorBase*>(lhs.opt));
            if (vec->empty())
                ctx->throw_exception("Indexing an empty vector variable", lhs.it_range);
            if (lhs.index >= int(vec->size()))
                ctx->throw_exception("Index out of range", lhs.it_range);
            switch (lhs.opt->type()) {
            case coFloats:
                check_numeric(rhs);
                static_cast<ConfigOptionFloats*>(vec)->values[lhs.index] = rhs.as_d();
                break;
            case coInts:
                check_numeric(rhs);
                static_cast<ConfigOptionInts*>(vec)->values[lhs.index] = rhs.as_i();
                break;
            case coStrings:
                static_cast<ConfigOptionStrings*>(vec)->values[lhs.index] = rhs.to_string();
                break;
            case coPercents:
                check_numeric(rhs);
                static_cast<ConfigOptionPercents*>(vec)->values[lhs.index] = rhs.as_d();
                break;
            case coBools:
                if (rhs.type() != expr::TYPE_BOOL)
                    ctx->throw_exception("Right side is not a boolean expression", rhs.it_range);
                static_cast<ConfigOptionBools*>(vec)->values[lhs.index] = rhs.b();
                break;
            default:
                ctx->throw_exception("Unsupported output vector variable type", lhs.it_range);
            }
        }

        static void vector_variable_assign_expr_with_count(const MyContext *ctx, OptWithPos &lhs, const expr &rhs_count, const expr &rhs_value)
        {
            assert(! ctx->skipping());
            size_t count = evaluate_count(rhs_count);
            auto *opt = const_cast<ConfigOption*>(lhs.opt);
            switch (lhs.opt->type()) {
            case coFloats:
                check_numeric(rhs_value);
                static_cast<ConfigOptionFloats*>(opt)->values.assign(count, rhs_value.as_d());
                break;
            case coInts:
                check_numeric(rhs_value);
                static_cast<ConfigOptionInts*>(opt)->values.assign(count, rhs_value.as_i());
                break;
            case coStrings:
                static_cast<ConfigOptionStrings*>(opt)->values.assign(count, rhs_value.to_string());
                break;
            case coBools:
                if (rhs_value.type() != expr::TYPE_BOOL)
                    rhs_value.throw_exception("Right side is not a boolean expression");
                static_cast<ConfigOptionBools*>(opt)->values.assign(count, rhs_value.b());
                break;
            default: assert(false);
            }
        }

        static void variable_value(const MyContext *ctx, OptWithPos &opt, expr &output)
        {
            if (! ctx->skipping()) {
                if (opt.opt->is_vector())
                    vector_element_to_expr(ctx, opt, output);
                else
                    scalar_variable_to_expr(ctx, opt, output);
            }
            output.it_range = opt.it_range;
        }

        // Return a boolean value, true if the scalar variable referenced by "opt" is nullable and it has a nil value.
        // Return a boolean value, true if an element of a vector variable referenced by "opt[index]" is nullable and it has a nil value.
        static void is_nil_test(const MyContext *ctx, OptWithPos &opt, expr &output)
        {
            if (ctx->skipping()) {
            } else if (opt.opt->is_vector()) {
                if (! opt.has_index())
                    ctx->throw_exception("Referencing a vector variable when scalar is expected", opt.it_range);
                const ConfigOptionVectorBase *vec = static_cast<const ConfigOptionVectorBase*>(opt.opt);
                if (vec->empty())
                    ctx->throw_exception("Indexing an empty vector variable", opt.it_range);
                output.set_b(static_cast<const ConfigOptionVectorBase*>(opt.opt)->is_nil(opt.index >= int(vec->size()) ? 0 : size_t(opt.index)));
            } else {
                assert(opt.opt->is_scalar());
                output.set_b(opt.opt->is_nil());
            }
            output.it_range = opt.it_range;
        }

        // Reference to an existing symbol, or a name of a new symbol.
        struct NewOldVariable {
            std::string    name;
            IteratorRange  it_range;
            ConfigOption  *opt{ nullptr };
        };
        static void new_old_variable(
            const MyContext       *ctx,
            bool                   global_variable,
            const IteratorRange   &it_range,
            NewOldVariable        &out)
        {
            if (! ctx->skipping()) {
                t_config_option_key key(std::string(it_range.begin(), it_range.end()));
                if (const ConfigOption* opt = ctx->resolve_symbol(key); opt)
                    ctx->throw_exception("Symbol is already defined in read-only system dictionary", it_range);
                if (ctx->config_outputs && ctx->config_outputs->optptr(key))
                    ctx->throw_exception("Symbol is already defined as system output variable", it_range);
                bool has_global_dictionary = ctx->context_data != nullptr && ctx->context_data->global_config;
                if (global_variable) {
                    if (! has_global_dictionary)
                        ctx->throw_exception("Global variables are not available in this context", it_range);
                    if (ctx->config_local.optptr(key))
                        ctx->throw_exception("Variable name already defined in local scope", it_range);
                    out.opt = ctx->context_data->global_config->optptr(key);
                } else {
                    if (has_global_dictionary && ctx->context_data->global_config->optptr(key))
                        ctx->throw_exception("Variable name already defined in global scope", it_range);
                    out.opt = ctx->config_local.optptr(key);
                }
                out.name     = std::move(key);
            }
            out.it_range = it_range;
        }

        // Decoding a scalar variable symbol "opt", assigning it a value of "param".
        static void scalar_variable_assign_scalar_expression(const MyContext *ctx, OptWithPos &opt, const expr &param)
        {
            if (! ctx->skipping()) {
                check_writable(ctx, opt);
                if (opt.opt->is_vector())
                    vector_variable_element_assign_scalar(ctx, opt, param);
                else
                    scalar_variable_assign_scalar(ctx, opt, param);
            }
        }

        static void scalar_variable_new_from_scalar_expression(
            const MyContext *ctx,
            bool             global_variable,
            NewOldVariable  &lhs,
            const expr      &rhs)
        {
            if (ctx->skipping()) {
            } else if (lhs.opt) {
                if (lhs.opt->is_vector())
                    rhs.throw_exception("Cannot assign a scalar value to a vector variable.");
                OptWithPos lhs_opt{ lhs.opt, lhs.it_range, true };
                scalar_variable_assign_scalar(ctx, lhs_opt, rhs);
            } else {
                std::unique_ptr<ConfigOption> opt_new;
                switch (rhs.type()) {
                    case expr::TYPE_BOOL:     opt_new = std::make_unique<ConfigOptionBool>(rhs.b());   break;
                    case expr::TYPE_INT:      opt_new = std::make_unique<ConfigOptionInt>(rhs.i());    break;
                    case expr::TYPE_DOUBLE:   opt_new = std::make_unique<ConfigOptionFloat>(rhs.d());  break;
                    case expr::TYPE_STRING:   opt_new = std::make_unique<ConfigOptionString>(rhs.s()); break;
                    default: assert(false);
                }
                const_cast<MyContext*>(ctx)->store_new_variable(lhs.name, std::move(opt_new), global_variable);
            }
        }

        static void vector_variable_new_from_array(
            const MyContext     *ctx,
            bool                 global_variable,
            NewOldVariable      &lhs,
            const expr          &rhs_count,
            const expr          &rhs_value)
        {
            if (ctx->skipping()) {
            } else if (lhs.opt) {
                if (lhs.opt->is_scalar())
                    rhs_value.throw_exception("Cannot assign a vector value to a scalar variable.");
                OptWithPos lhs_opt{ lhs.opt, lhs.it_range, true };
                vector_variable_assign_expr_with_count(ctx, lhs_opt, rhs_count, rhs_value);
            } else {
                size_t count = evaluate_count(rhs_count);
                std::unique_ptr<ConfigOption> opt_new;
                switch (rhs_value.type()) {
                    case expr::TYPE_BOOL:     opt_new = std::make_unique<ConfigOptionBools>(count, rhs_value.b());   break;
                    case expr::TYPE_INT:      opt_new = std::make_unique<ConfigOptionInts>(count, rhs_value.i());    break;
                    case expr::TYPE_DOUBLE:   opt_new = std::make_unique<ConfigOptionFloats>(count, rhs_value.d()); break;
                    case expr::TYPE_STRING:   opt_new = std::make_unique<ConfigOptionStrings>(count, rhs_value.s()); break;
                    default: assert(false);
                }
                const_cast<MyContext*>(ctx)->store_new_variable(lhs.name, std::move(opt_new), global_variable);
            }
        }

        static void vector_variable_assign_array(
            const MyContext *ctx,
            OptWithPos      &lhs,
            const expr      &rhs_count,
            const expr      &rhs_value)
        {
            if (! ctx->skipping()) {
                check_writable(ctx, lhs);
                if (lhs.opt->is_scalar())
                    rhs_value.throw_exception("Cannot assign a vector value to a scalar variable.");
                vector_variable_assign_expr_with_count(ctx, lhs, rhs_count, rhs_value);
            }
        }

        template<typename ConfigOptionType, typename RightValueEvaluate>
        static void fill_vector_from_initializer_list(ConfigOption *opt, const std::vector<expr> &il, RightValueEvaluate rv_eval) {
            auto& out = static_cast<ConfigOptionType*>(opt)->values;
            out.clear();
            out.reserve(il.size());
            for (const expr& i : il)
                out.emplace_back(rv_eval(i));
        }

        static void vector_variable_assign_initializer_list(const MyContext *ctx, OptWithPos &lhs, const std::vector<expr> &il)
        {
            if (ctx->skipping())
                return;

            check_writable(ctx, lhs);

            if (lhs.opt->is_scalar()) {
                if (il.size() == 1)
                    // scalar_var = ( scalar )
                    scalar_variable_assign_scalar_expression(ctx, lhs, il.front());
                else
                    // scalar_var = () 
                    // or
                    // scalar_var = ( scalar, scalar, ... )
                    ctx->throw_exception("Cannot assign a vector value to a scalar variable.", lhs.it_range);
            }

            auto check_numeric_vector = [](const std::vector<expr> &il) {
                for (auto &i : il)
                    if (! i.numeric_type())
                        i.throw_exception("Right side is not a numeric expression");
            };

            ConfigOption *opt = const_cast<ConfigOption*>(lhs.opt);
            switch (lhs.opt->type()) {
            case coFloats:
                check_numeric_vector(il);
                fill_vector_from_initializer_list<ConfigOptionFloats>(opt, il, [](auto &v){ return v.as_d(); });
                break;
            case coInts:
                check_numeric_vector(il);
                fill_vector_from_initializer_list<ConfigOptionInts>(opt, il, [](auto &v){ return v.as_i(); });
                break;
            case coStrings:
                fill_vector_from_initializer_list<ConfigOptionStrings>(opt, il, [](auto &v){ return v.to_string(); });
                break;
            case coBools:
                for (auto &i : il)
                    if (i.type() != expr::TYPE_BOOL)
                        i.throw_exception("Right side is not a boolean expression");
                fill_vector_from_initializer_list<ConfigOptionBools>(opt, il, [](auto &v){ return v.b(); });
                break;
            default: assert(false);
            }
        }

        static void vector_variable_new_from_initializer_list(
            const MyContext         *ctx,
            bool                     global_variable,
            NewOldVariable          &lhs,
            const std::vector<expr> &il)
        {
            if (ctx->skipping())
                return;

            if (lhs.opt) {
                // Assign to an existing vector variable.
                OptWithPos lhs_opt{ lhs.opt, lhs.it_range, true };
                vector_variable_assign_initializer_list(ctx, lhs_opt, il);
            } else {
                if (il.empty())
                    ctx->throw_exception("Cannot create vector variable from an empty initializer list, because its type cannot be deduced.", lhs.it_range);
                // Allocate a new vector variable.
                // First guesstimate type of the output vector.
                size_t num_bool = 0;
                size_t num_int = 0;
                size_t num_double = 0;
                size_t num_string = 0;
                for (auto &i : il)
                    switch (i.type()) {
                    case expr::TYPE_BOOL:     ++ num_bool;    break;
                    case expr::TYPE_INT:      ++ num_int;     break;
                    case expr::TYPE_DOUBLE:   ++ num_double;  break;
                    case expr::TYPE_STRING:   ++ num_string;  break;
                    default: assert(false);
                    }
                std::unique_ptr<ConfigOption> opt_new;
                if (num_string > 0)
                    // Convert everything to strings.
                    opt_new = std::make_unique<ConfigOptionStrings>();
                else if (num_bool > 0) {
                    if (num_double + num_int > 0)
                        ctx->throw_exception("Right side is not valid: Mixing numeric and boolean types.", IteratorRange{ il.front().it_range.begin(), il.back().it_range.end() });
                    opt_new = std::make_unique<ConfigOptionBools>();
                } else {
                    // Output is numeric.
                    if (num_double == 0)
                        opt_new = std::make_unique<ConfigOptionInts>();
                    else 
                        opt_new = std::make_unique<ConfigOptionFloats>();
                }
                OptWithPos lhs_opt{ opt_new.get(), lhs.it_range, true };
                vector_variable_assign_initializer_list(ctx, lhs_opt, il);
                const_cast<MyContext*>(ctx)->store_new_variable(lhs.name, std::move(opt_new), global_variable);
            }
        }

        static bool is_vector_variable_reference(const OptWithPos &var) {
            return ! var.empty() && ! var.has_index() && var.opt->is_vector();
        }

        // Called when checking whether the NewOldVariable could be assigned a vectir right hand side.
        static bool could_be_vector_variable_reference(const NewOldVariable &var) {
            return var.opt == nullptr || var.opt->is_vector();
        }

        static void copy_vector_variable_to_vector_variable(const MyContext *ctx, OptWithPos &lhs, const OptWithPos &rhs)
        {
            if (ctx->skipping())
                return;

            check_writable(ctx, lhs);
            assert(lhs.opt->is_vector());
            if (rhs.has_index() || ! rhs.opt->is_vector())
                ctx->throw_exception("Cannot assign scalar to a vector", lhs.it_range);
            if (rhs.opt->is_nil())
                ctx->throw_exception("Some elements of the right hand side vector variable of optional values are undefined (nil)", rhs.it_range);
            if (lhs.opt->type() != rhs.opt->type()) {
                // Vector types are not compatible.
                switch (lhs.opt->type()) {
                case coFloats:
                    ctx->throw_exception("Left hand side is a float vector, while the right hand side is not.", lhs.it_range);
                case coInts:
                    ctx->throw_exception("Left hand side is an int vector, while the right hand side is not.", lhs.it_range);
                case coStrings:
                    ctx->throw_exception("Left hand side is a string vector, while the right hand side is not.", lhs.it_range);
                case coBools:
                    ctx->throw_exception("Left hand side is a bool vector, while the right hand side is not.", lhs.it_range);
                default:
                    ctx->throw_exception("Left hand side / right hand side vectors are not compatible.", lhs.it_range);
                }
            }
            const_cast<ConfigOption*>(lhs.opt)->set(rhs.opt);
        }

        static bool vector_variable_new_from_copy(
            const MyContext   *ctx,
            bool               global_variable,
            NewOldVariable    &lhs,
            const OptWithPos  &rhs)
        {
            if (ctx->skipping())
                // Skipping, continue parsing.
                return true;

            if (lhs.opt) {
                assert(lhs.opt->is_vector());
                OptWithPos lhs_opt{ lhs.opt, lhs.it_range, true };
                copy_vector_variable_to_vector_variable(ctx, lhs_opt, rhs);
            } else {
                if (rhs.has_index() || ! rhs.opt->is_vector())
                    // Stop parsing, let the other rules resolve this case.
                    return false;
                if (rhs.opt->is_nil())
                    ctx->throw_exception("Some elements of the right hand side vector variable of optional values are undefined (nil)", rhs.it_range);
                // Clone the vector variable.
                std::unique_ptr<ConfigOption> opt_new;
                if (one_of(rhs.opt->type(), { coFloats, coInts, coStrings, coBools }))
                    opt_new = std::unique_ptr<ConfigOption>(rhs.opt->clone());
                else if (rhs.opt->type() == coPercents)
                    opt_new = std::make_unique<ConfigOptionFloats>(static_cast<const ConfigOptionPercents*>(rhs.opt)->values);
                else
                    ctx->throw_exception("Duplicating this type of vector variable is not supported", rhs.it_range);
                const_cast<MyContext*>(ctx)->store_new_variable(lhs.name, std::move(opt_new), global_variable);
            }
            // Continue parsing.
            return true;
        }

        static void initializer_list_append(std::vector<expr> &list, expr &param)
        {
            if (param.type() != expr::TYPE_EMPTY)
                // not skipping
                list.emplace_back(std::move(param));
        }

        static void is_vector_empty(const MyContext *ctx, OptWithPos &opt, expr &out)
        {
            if (! ctx->skipping()) {
                if (opt.has_index() || ! opt.opt->is_vector())
                    ctx->throw_exception("parameter of empty() is not a vector variable", opt.it_range);
                out.set_b(static_cast<const ConfigOptionVectorBase*>(opt.opt)->size() == 0);
            }
            out.it_range = opt.it_range;
        }

        static void vector_size(const MyContext *ctx, OptWithPos &opt, expr &out)
        {
            if (! ctx->skipping()) {
                if (opt.has_index() || ! opt.opt->is_vector())
                    ctx->throw_exception("parameter of size() is not a vector variable", opt.it_range);
                out.set_i(int(static_cast<const ConfigOptionVectorBase*>(opt.opt)->size()));
            }
            out.it_range = opt.it_range;
        }

        // Verify that the expression returns an integer, which may be used
        // to address a vector.
        static void evaluate_index(expr &expr_index, int &output)
        {
            if (expr_index.type() != expr::TYPE_EMPTY) {
                if (expr_index.type() != expr::TYPE_INT)
                    expr_index.throw_exception("Non-integer index is not allowed to address a vector variable.");
                output = expr_index.i();
            }
        }

        static void random(const MyContext *ctx, expr &param1, expr &param2)
        {
            if (ctx->skipping())
                return;

            if (ctx->context_data == nullptr)
                ctx->throw_exception("Random number generator not available in this context.",
                    IteratorRange(param1.it_range.begin(), param2.it_range.end()));
            expr::random(param1, param2, ctx->context_data->rng);
        }

        static void filament_change(const MyContext* ctx, expr& param)
        {
            MyContext *context = const_cast<MyContext *>(ctx);
            context->current_extruder_id = param.as_i();
        }

        static void throw_exception(const std::string &msg, const IteratorRange &it_range)
        {
            // An asterix is added to the start of the string to differentiate the boost::spirit::info::tag content
            // between the grammer terminal / non-terminal symbol name and a free-form error message.
            boost::throw_exception(qi::expectation_failure(it_range.begin(), it_range.end(), spirit::info(std::string("*") + msg)));
        }

        static void process_error_message(const MyContext *context, const boost::spirit::info &info, const Iterator &it_begin, const Iterator &it_end, const Iterator &it_error)
        {
            std::string &msg = const_cast<MyContext*>(context)->error_message;
            std::string  first(it_begin, it_error);
            std::string  last(it_error, it_end);
            auto         first_pos  = first.rfind('\n');
            auto         last_pos   = last.find('\n');
            int          line_nr    = 1;
            if (first_pos == std::string::npos)
                first_pos = 0;
            else {
                // Calculate the current line number.
                for (size_t i = 0; i <= first_pos; ++ i)
                    if (first[i] == '\n')
                        ++ line_nr;
                ++ first_pos;
            }
            auto error_line = std::string(first, first_pos) + std::string(last, 0, last_pos);
            // Position of the it_error from the start of its line.
            auto error_pos  = (it_error - it_begin) - first_pos;
            msg += "Parsing error at line " + std::to_string(line_nr);
            if (! info.tag.empty() && info.tag.front() == '*') {
                // The gat contains an explanatory string.
                msg += ": ";
                msg += info.tag.substr(1);
            } else {
                auto it = tag_to_error_message.find(info.tag);
                if (it == tag_to_error_message.end()) {
                    // A generic error report based on the nonterminal or terminal symbol name.
                    msg += ". Expecting tag ";
                    msg += info.tag;
                } else {
                    // Use the human readable error message.
                    msg += ". ";
                    msg += it->second;
                }
            }
            msg += '\n';
            // This hack removes all non-UTF8 characters from the source line, so that the upstream wxWidgets conversions
            // from UTF8 to UTF16 don't bail out.
            msg += boost::nowide::narrow(boost::nowide::widen(error_line));
            msg += '\n';
            for (size_t i = 0; i < error_pos; ++ i)
                msg += ' ';
            msg += "^\n";
        }

    private:
        // For skipping execution of inactive conditional branches.
        mutable int m_depth_suppressed{ 0 };
    };

    struct InterpolateTableContext {
        struct Item {
            double         x;
            IteratorRange  it_range_x;
            double         y;
        };
        std::vector<Item> table;

        static void init(const expr &x) {
            if (x.type() != expr::TYPE_EMPTY) {
                if (!x.numeric_type())
                    x.throw_exception("Interpolation value must be a number.");
            }
        }
        static void add_pair(const expr &x, const expr &y, InterpolateTableContext &table) {
            if (x.type() != expr::TYPE_EMPTY) {
                if (! x.numeric_type())
                    x.throw_exception("X value of a table point must be a number.");
                if (! y.numeric_type())
                    y.throw_exception("Y value of a table point must be a number.");
                table.table.push_back({ x.as_d(), x.it_range, y.as_d() });
            }
        }
        static void evaluate(const expr &expr_x, const InterpolateTableContext &table, expr &out) {
            if (expr_x.type() == expr::TYPE_EMPTY)
                return;

            // Check whether the table X values are sorted.
            double x = expr_x.as_d();
            assert(! std::isnan(x));
            bool   evaluated = false;
            for (size_t i = 1; i < table.table.size(); ++i) {
                double x0 = table.table[i - 1].x;
                double x1 = table.table[i].x;
                if (x0 > x1)
                    boost::throw_exception(qi::expectation_failure(
                        table.table[i - 1].it_range_x.begin(), table.table[i].it_range_x.end(), spirit::info("X coordinates of the table must be increasing")));
                if (! evaluated && x >= x0 && x <= x1) {
                    double y0 = table.table[i - 1].y;
                    double y1 = table.table[i].y;
                    if (x == x0)
                        out.set_d(y0);
                    else if (x == x1)
                        out.set_d(y1);
                    else if (is_approx(x0, x1))
                        out.set_d(0.5 * (y0 + y1));
                    else
                        out.set_d(Slic3r::lerp(y0, y1, (x - x0) / (x1 - x0)));
                    evaluated = true;
                }
            }
            if (! evaluated) {
                // Clamp x into the table range with EPSILON.
                if (double x0 = table.table.front().x; x > x0 - EPSILON && x < x0)
                    out.set_d(table.table.front().y);
                else if (double x1 = table.table.back().x; x > x1 && x < x1 + EPSILON)
                    out.set_d(table.table.back().y);
                else
                    // The value is really outside the table range.
                    expr_x.throw_exception("Interpolation value is outside the table range");
            }
        }
    };

    std::ostream& operator<<(std::ostream &os, const InterpolateTableContext &table_context)
    {
        for (const auto &item : table_context.table)
            os << "(" << item.x << "," << item.y << ")";
        return os;
    }

    // Table to translate symbol tag to a human readable error message.
    std::map<std::string, std::string> MyContext::tag_to_error_message = {
        { "eoi",                        "Unknown syntax error" },
        { "start",                      "Unknown syntax error" },
        { "text",                       "Invalid text." },
        { "text_block",                 "Invalid text block." },
        { "macro",                      "Invalid macro." },
        { "repeat",                     "Unknown syntax error" },
        { "if_else_output",             "Not an {if}{else}{endif} macro." },
        { "switch_output",              "Not a {switch} macro." },
        { "legacy_variable_expansion",  "Expecting a legacy variable expansion format" },
        { "identifier",                 "Expecting an identifier." },
        { "conditional_expression",     "Expecting a conditional expression." },
        { "logical_or_expression",      "Expecting a boolean expression." },
        { "logical_and_expression",     "Expecting a boolean expression." },
        { "equality_expression",        "Expecting an expression." },
        { "bool_expr_eval",             "Expecting a boolean expression."},
        { "relational_expression",      "Expecting an expression." },
        { "additive_expression",        "Expecting an expression." },
        { "multiplicative_expression",  "Expecting an expression." },
        { "unary_expression",           "Expecting an expression." },
        { "optional_parameter",         "Expecting a closing brace or an optional parameter." },
        { "one_of_list",                "Expecting a list of string patterns (simple text or rexep)" },
        { "variable_reference",         "Expecting a variable reference."},
        { "variable",                   "Expecting a variable name."},
        { "regular_expression",         "Expecting a regular expression."}
    };

    // For debugging the boost::spirit parsers. Print out the string enclosed in it_range.
    std::ostream& operator<<(std::ostream& os, const IteratorRange &it_range)
    {
        os << std::string(it_range.begin(), it_range.end());
        return os;
    }

    // Disable parsing int numbers (without decimals) and Inf/NaN symbols by the double parser.
    struct strict_real_policies_without_nan_inf : public qi::strict_real_policies<double>
    {
        template <typename It, typename Attr> static bool parse_nan(It&, It const&, Attr&) { return false; }
        template <typename It, typename Attr> static bool parse_inf(It&, It const&, Attr&) { return false; }
    };

    // This parser is to be used inside a raw[] directive to accept a single valid UTF-8 character.
    // If an invalid UTF-8 sequence is encountered, a qi::expectation_failure is thrown.
    struct utf8_char_parser : qi::primitive_parser<utf8_char_parser>
    {
        // Define the attribute type exposed by this parser component
        template <typename Context, typename Iterator>
        struct attribute
        {
            typedef wchar_t type;
        };

        // This function is called during the actual parsing process to skip whitespaces.
        // Also it throws if it encounters valid or invalid UTF-8 sequence.
        template <typename Iterator, typename Context , typename Skipper, typename Attribute>
        bool parse(Iterator &first, Iterator const &last, Context &context, Skipper const &skipper, Attribute& attr) const
        {
            // The skipper shall always be empty, any white space will be accepted.
            // skip_over(first, last, skipper);
            if (first == last)
                return false;
            // Iterator over the UTF-8 sequence.
            auto            it = first;
            // Read the first byte of the UTF-8 sequence.
            unsigned char   c  = static_cast<boost::uint8_t>(*it ++);
            unsigned int    cnt = 0;
            // UTF-8 sequence must not start with a continuation character:
            if ((c & 0xC0) == 0x80)
                goto err;
            // Skip high surrogate first if there is one.
            // If the most significant bit with a zero in it is in position
            // 8-N then there are N bytes in this UTF-8 sequence:
            {
                unsigned char mask   = 0x80u;
                unsigned int  result = 0;
                while (c & mask) {
                    ++ result;
                    mask >>= 1;
                }
                cnt = (result == 0) ? 1 : ((result > 4) ? 4 : result);
            }
            // Since we haven't read in a value, we need to validate the code points:
            for (-- cnt; cnt > 0; -- cnt) {
                if (it == last)
                    goto err;
                c = static_cast<boost::uint8_t>(*it ++);
                // We must have a continuation byte:
                if (cnt > 1 && (c & 0xC0) != 0x80)
                    goto err;
            }
            first = it;
            return true;
        err:
            MyContext::throw_exception("Invalid utf8 sequence", IteratorRange(first, last));
            return false;
        }

        // This function is called during error handling to create a human readable string for the error context.
        template <typename Context>
        spirit::info what(Context&) const
        {
            return spirit::info("unicode_char");
        }
    };

    // This parser is to be used inside a raw[] directive to accept a single valid UTF-8 character.
    // If an invalid UTF-8 sequence is encountered, a qi::expectation_failure is thrown.
    struct ascii_char_skipper_parser : public utf8_char_parser
    { 
        // This function is called during the actual parsing process 
        template <typename Iterator, typename Context, typename Skipper, typename Attribute>
        bool parse(Iterator &first, Iterator const &last, Context &context, Skipper const &skipper, Attribute &attr) const
        { 
            Iterator it = first;
            // Let the UTF-8 parser throw if it encounters an invalid UTF-8 sequence. 
            if (! utf8_char_parser::parse(it, last, context, skipper, attr))
                return false;
            char c = *first;
            if (it - first > 1 || c < 0)
                MyContext::throw_exception("Non-ASCII7 characters are only allowed inside text blocks and string literals, not inside code blocks.", IteratorRange(first, it));
            if (c == '\r' || c == '\n' || c == '\t' || c == ' ') {
                // Skip the whitespaces
                ++ first;
                return true;
            } else
                // Stop skipping, let this 7bit ASCII character be processed.
                return false;
        }

        // This function is called during error handling to create a human readable string for the error context.
        template <typename Context>
        spirit::info what(Context&) const
        { 
            return spirit::info("ASCII7_char");
        }
    };

    struct FactorActions {
        static void set_start_pos(Iterator &start_pos, expr &out)
                { out.it_range = IteratorRange(start_pos, start_pos); }
        static void int_(const MyContext *ctx, int &value, Iterator &end_pos, expr &out) { 
            if (ctx->skipping()) {
                out.reset();
                out.it_range.end() = end_pos;
            } else
                out = expr(value, out.it_range.begin(), end_pos);
        }
        static void double_(const MyContext *ctx, double &value, Iterator &end_pos, expr &out) { 
            if (ctx->skipping()) {
                out.reset();
                out.it_range.end() = end_pos;
            } else
                out = expr(value, out.it_range.begin(), end_pos);
        }
        static void bool_(const MyContext *ctx, bool &value, Iterator &end_pos, expr &out) { 
            if (ctx->skipping()) {
                out.reset();
                out.it_range.end() = end_pos;
            } else
                out = expr(value, out.it_range.begin(), end_pos);
        }
        static void string_(const MyContext *ctx, IteratorRange &it_range, expr &out) { 
            if (ctx->skipping()) {
                out.reset();
                out.it_range = it_range;
            } else {
                // Unescape the string, UTF-8 safe.
                std::string s;
                auto        begin = std::next(it_range.begin());
                auto        end   = std::prev(it_range.end());
                assert(begin <= end);
                {
                    // 1) Get the size of the string after unescaping.
                    size_t len = 0;
                    for (auto it = begin; it != end;) {
                        if (*it == '\\') {
                            if (++ it == end ||
                                (*it != 'r' && *it != 'n' && *it != '"' && *it != '\\'))
                                ctx->throw_exception("Invalid escape sequence", {std::prev(it), std::next(it) });
                            ++ len;
                            ++ it;
                        } else {
                            size_t n = get_utf8_sequence_length(&*it, end - it);
                            len += n;
                            it  += n;
                        }
                    }
                    // and reserve the string.
                    s.reserve(len);
                }
                // 2) Copy & unescape the string.
                for (auto it = begin; it != end;) {
                    if (*it == '\\') {
                        char c = *(++ it);
                        if (c == 'r')
                            c = '\r';
                        else if (c == 'n')
                            c = '\n';
                        s += c;
                        ++ it;
                    } else {
                        size_t n = get_utf8_sequence_length(&*it, end - it);
                        s.append(&*it, n);
                        it += n;
                    }
                }
                out = expr(std::move(s), it_range.begin(), it_range.end());
            }
        }
        static void expr_(expr &value, Iterator &end_pos, expr &out)
                { auto begin_pos = out.it_range.begin(); out = expr(std::move(value), begin_pos, end_pos); }
        static void minus_(expr &value, expr &out)
                { out = value.unary_minus(out.it_range.begin()); }
        static void not_(expr &value, expr &out)
                { out = value.unary_not(out.it_range.begin()); }
        static void to_int(expr &value, expr &out)
                { out = value.unary_integer(out.it_range.begin()); }
        static void round(expr &value, expr &out)
                { out = value.round(out.it_range.begin()); }
        static void floor(expr &value, expr &out)
                { out = value.floor(out.it_range.begin()); }
        static void ceil(expr &value, expr &out)
                { out = value.ceil(out.it_range.begin());}
        // For indicating "no optional parameter".
        static void noexpr(expr &out) { out.reset(); }
    };

    using skipper = ascii_char_skipper_parser;

    ///////////////////////////////////////////////////////////////////////////
    //  Our macro_processor grammar
    ///////////////////////////////////////////////////////////////////////////
    // Inspired by the C grammar rules https://www.lysator.liu.se/c/ANSI-C-grammar-y.html
    struct macro_processor : qi::grammar<Iterator, std::string(const MyContext*), qi::locals<bool>, skipper>
    {
        macro_processor() : macro_processor::base_type(start)
        {
            using namespace qi::labels;
            qi::alpha_type              alpha;
            qi::alnum_type              alnum;
            qi::eps_type                eps;
            qi::raw_type                raw;
            qi::lit_type                lit;
            qi::lexeme_type             lexeme;
            qi::no_skip_type            no_skip;
            qi::real_parser<double, strict_real_policies_without_nan_inf> strict_double;
            spirit_encoding::char_type  char_;
            utf8_char_parser            utf8char;
            spirit::bool_type           bool_;
            spirit::int_type            int_;
            spirit::double_type         double_;
            spirit_encoding::string_type string;
			spirit::eoi_type			eoi;
			spirit::repository::qi::iter_pos_type iter_pos;
            auto                        kw = spirit::repository::qi::distinct(qi::copy(alnum | '_'));

            qi::_val_type               _val;
            qi::_1_type                 _1;
            qi::_2_type                 _2;
            qi::_3_type                 _3;
            qi::_4_type                 _4;
            qi::_a_type                 _a;
            qi::_b_type                 _b;
            qi::_r1_type                _r1;
            qi::_r2_type                _r2;

            // Starting symbol of the grammer.
            // The leading eps is required by the "expectation point" operator ">".
            // Without it, some of the errors would not trigger the error handler.
            // Also the start symbol switches between the "full macro syntax" and a "boolean expression only",
            // depending on the context->just_boolean_expression flag. This way a single static expression parser
            // could serve both purposes.
            start =
                (       (eps(px::bind(&MyContext::evaluate_full_macro, _r1)) > text_block(_r1) [_val=_1])
                    |   conditional_expression(_r1) [ px::bind(&expr::evaluate_boolean_to_string, _1, _val) ]
				) > eoi;
            start.name("start");
            qi::on_error<qi::fail>(start, px::bind(&MyContext::process_error_message, _r1, _4, _1, _2, _3));

            text_block = *(
                        text [_val+=_1]
                        // Allow back tracking after '{' in case of a text_block embedded inside a condition.
                        // In that case the inner-most {else} wins and the {if}/{elsif}/{else} shall be paired.
                        // {elsif}/{else} without an {if} will be allowed to back track from the embedded text_block.
                    |   (lit('{') >> (macros(_r1)[_val += _1] > '}') | '}')
                    |   (lit('[') > legacy_variable_expansion(_r1) [_val+=_1] > ']')
                );
            text_block.name("text_block");

            // Free-form text up to a first brace, including spaces and newlines.
            // The free-form text will be inserted into the processed text without a modification.
            text = no_skip[raw[+(utf8char - char_('[') - char_('{'))]];
            text.name("text");

            // New style of macro expansion.
            // The macro expansion may contain numeric or string expressions, ifs and cases.
            macros =
                +(block(_r1)[_val += _1] | (statement(_r1) > (+lit(';') | &lit('}')))[_val += _1] | +lit(';'));
            macros.name("macro");
            // if_macros and else_macros only differ by the look-ahead ending condition, which is to not have to repeat the last semicolon
            // at the end of the block.
            if_macros = kw["then"] > *(block(_r1)[_val += _1] | (statement(_r1) > (+lit(';') | &(kw["elsif"] | kw["else"] | kw["endif"])))[_val += _1] | +lit(';'));
            if_macros.name("if_macros");
            else_macros = *(block(_r1)[_val += _1] | (statement(_r1) > (+lit(';') | &kw["endif"]))[_val += _1] | +lit(';'));
            else_macros.name("else_macros");

            // Blocks do not require a separating semicolon.
            block = 
                    (kw["if"] > if_else_output(_r1)[_val = _1])
                // (kw["switch"] ...
                ;
            block.name("block");

            // Statements require a separating semicolon.
            statement =
                  (assignment_statement(_r1)  [_val = _1])
                | (new_variable_statement(_r1)[_val = _1])
                | (conditional_expression(_r1)[px::bind(&expr::to_string2, _1, _val)])
                ;

            // An if expression enclosed in {} (the outmost {} are already parsed by the caller).
            // Also }{ could be replaced with ; to simplify writing of pure code.
            if_else_output =
                eps[_a=true] >
                (bool_expr_eval(_r1)[px::bind(&MyContext::block_enter, _r1, _1)] > (if_text_block(_r1) | if_macros(_r1)))
                    [px::bind(&MyContext::block_exit, _r1, _1, _a, _2, _val)] >
                *((kw["elsif"] > bool_expr_eval(_r1)[px::bind(&MyContext::block_enter, _r1, _1 && _a)] > (if_text_block(_r1) | if_macros(_r1)))
                    [px::bind(&MyContext::block_exit, _r1, _1, _a, _2, _val)]) >
                -(kw["else"] > eps[px::bind(&MyContext::block_enter, _r1, _a)] > (if_text_block(_r1) | else_macros(_r1)))
                    [px::bind(&MyContext::block_exit, _r1, _a, _a, _1, _val)] >
                kw["endif"];
            if_else_output.name("if_else_output");
            if_text_block = (lit('}') > text_block(_r1) > '{');
            if_text_block.name("if_text_block");

            // A switch expression enclosed in {} (the outmost {} are already parsed by the caller).
/*
            switch_output =
                eps[_b=true] >
                omit[expr(_r1)[_a=_1]] > '}' > text_block(_r1)[px::bind(&expr::set_if_equal, _a, _b, _1, _val)] > '{' >
                *("elsif" > omit[bool_expr_eval(_r1)[_a=_1]] > '}' > text_block(_r1)[px::bind(&expr::set_if, _a, _b, _1, _val)]) >>
                -("else" > '}' >> text_block(_r1)[px::bind(&expr::set_if, _b, _b, _1, _val)]) >
                "endif";
*/

            // Legacy variable expansion of the original Slic3r, in the form of [scalar_variable] or [vector_variable_index].
            legacy_variable_expansion =
                    (identifier >> &lit(']'))
                        [ px::bind(&MyContext::legacy_variable_expansion, _r1, _1, _val) ]
                |   (identifier > lit('[') > identifier > ']')
                        [ px::bind(&MyContext::legacy_variable_expansion2, _r1, _1, _2, _val) ]
                ;
            legacy_variable_expansion.name("legacy_variable_expansion");

            identifier =
                ! kw[keywords] >>
                raw[lexeme[(alpha | '_') >> *(alnum | '_')]];
            identifier.name("identifier");

            conditional_expression =
                logical_or_expression(_r1) [_val = _1]
                >> -('?' > eps[px::bind(&expr::evaluate_boolean, _val, _a)] >
                      eps[px::bind(&MyContext::block_enter, _r1, _a)] > conditional_expression(_r1)[px::bind(&MyContext::block_exit_ternary, _r1, _a, _1, _val)]
                      > ':' >
                    eps[px::bind(&MyContext::block_enter, _r1, ! _a)] > conditional_expression(_r1)[px::bind(&MyContext::block_exit_ternary, _r1, ! _a, _1, _val)]);
            conditional_expression.name("conditional_expression");

            logical_or_expression =
                logical_and_expression(_r1)                [_val = _1]
                >> *(   ((kw["or"] | "||") > logical_and_expression(_r1) ) [px::bind(&expr::logical_or, _val, _1)] );
            logical_or_expression.name("logical_or_expression");

            logical_and_expression =
                equality_expression(_r1)                   [_val = _1]
                >> *(   ((kw["and"] | "&&") > equality_expression(_r1) ) [px::bind(&expr::logical_and, _val, _1)] );
            logical_and_expression.name("logical_and_expression");

            equality_expression =
                relational_expression(_r1)                   [_val = _1]
                >> *(   ("==" > relational_expression(_r1) ) [px::bind(&expr::equal,     _val, _1)]
                    |   ("!=" > relational_expression(_r1) ) [px::bind(&expr::not_equal, _val, _1)]
                    |   ("<>" > relational_expression(_r1) ) [px::bind(&expr::not_equal, _val, _1)]
                    |   ("=~" > regular_expression         ) [px::bind(&expr::regex_matches, _val, _1)]
                    |   ("!~" > regular_expression         ) [px::bind(&expr::regex_doesnt_match, _val, _1)]
                    );
            equality_expression.name("bool expression");

            // Evaluate a boolean expression stored as expr into a boolean value.
            // Throw if the equality_expression does not produce a expr of boolean type.
            bool_expr_eval = conditional_expression(_r1) [ px::bind(&expr::evaluate_boolean, _1, _val) ];
            bool_expr_eval.name("bool_expr_eval");

            relational_expression =
                    additive_expression(_r1)                [_val  = _1]
                >> *(   ("<="     > additive_expression(_r1) ) [px::bind(&expr::leq,     _val, _1)]
                    |   (">="     > additive_expression(_r1) ) [px::bind(&expr::geq,     _val, _1)]
                    |   (lit('<') > additive_expression(_r1) ) [px::bind(&expr::lower,   _val, _1)]
                    |   (lit('>') > additive_expression(_r1) ) [px::bind(&expr::greater, _val, _1)]
                    );
            relational_expression.name("relational_expression");

            additive_expression =
                multiplicative_expression(_r1)                       [_val  = _1]
                >> *(   (lit('+') > multiplicative_expression(_r1) ) [_val += _1]
                    |   (lit('-') > multiplicative_expression(_r1) ) [_val -= _1]
                    );
            additive_expression.name("additive_expression");

            multiplicative_expression =
                unary_expression(_r1)                       [_val  = _1]
                >> *(   (lit('*') > unary_expression(_r1) ) [_val *= _1]
                    |   (lit('/') > unary_expression(_r1) ) [_val /= _1]
                    |   (lit('%') > unary_expression(_r1) ) [_val %= _1]
                    );
            multiplicative_expression.name("multiplicative_expression");

            assignment_statement =
                (variable_reference(_r1)[_a = _1] >> '=') > 
                (       // Consumes also '(' conditional_expression ')', that means enclosing an expression into braces makes it a single value vector initializer.
                         initializer_list(_r1)[px::bind(&MyContext::vector_variable_assign_initializer_list, _r1, _a, _1)]
                        // Process it before conditional_expression, as conditional_expression requires a vector reference to be augmented with an index.
                        // Only process such variable references, which return a naked vector variable.
                    |  eps(px::bind(&MyContext::is_vector_variable_reference, _a)) >> 
                            variable_reference(_r1)[px::bind(&MyContext::copy_vector_variable_to_vector_variable, _r1, _a, _1)]
                       // Would NOT consume '(' conditional_expression ')' because such value was consumed with the expression above.
                    |  conditional_expression(_r1)
                            [px::bind(&MyContext::scalar_variable_assign_scalar_expression, _r1, _a, _1)]
                    |  (kw["repeat"] > "(" > additive_expression(_r1) > "," > conditional_expression(_r1) > ")")
                            [px::bind(&MyContext::vector_variable_assign_array, _r1, _a, _1, _2)]
                );
  
            new_variable_statement =
                (kw["local"][_a = false] | kw["global"][_a = true]) > identifier[px::bind(&MyContext::new_old_variable, _r1, _a, _1, _b)] > lit('=') >
                (       // Consumes also '(' conditional_expression ')', that means enclosing an expression into braces makes it a single value vector initializer.
                        initializer_list(_r1)[px::bind(&MyContext::vector_variable_new_from_initializer_list, _r1, _a, _b, _1)]
                        // Process it before conditional_expression, as conditional_expression requires a vector reference to be augmented with an index.
                        // Only process such variable references, which return a naked vector variable.
                    |  eps(px::bind(&MyContext::could_be_vector_variable_reference, _b)) >>
                            variable_reference(_r1)[qi::_pass = px::bind(&MyContext::vector_variable_new_from_copy, _r1, _a, _b, _1)]
                       // Would NOT consume '(' conditional_expression ')' because such value was consumed with the expression above.
                    |  conditional_expression(_r1)
                            [px::bind(&MyContext::scalar_variable_new_from_scalar_expression, _r1, _a, _b, _1)]
                    |  (kw["repeat"] > "(" > additive_expression(_r1) > "," > conditional_expression(_r1) > ")")
                            [px::bind(&MyContext::vector_variable_new_from_array, _r1, _a, _b, _1, _2)]
                );
            initializer_list = lit('(') >
                (   lit(')') |
                    (   conditional_expression(_r1)[px::bind(&MyContext::initializer_list_append, _val, _1)] >
                        *(lit(',') > conditional_expression(_r1)[px::bind(&MyContext::initializer_list_append, _val, _1)]) >
                        lit(')')
                    )
                );

            unary_expression = iter_pos[px::bind(&FactorActions::set_start_pos, _1, _val)] >> (
                    variable_reference(_r1) [px::bind(&MyContext::variable_value, _r1, _1, _val)]
                |   (lit('(')  > conditional_expression(_r1) > ')' > iter_pos) [ px::bind(&FactorActions::expr_, _1, _2, _val) ]
                |   (lit('-')  > unary_expression(_r1)           )  [ px::bind(&FactorActions::minus_,  _1,     _val) ]
                |   (lit('+')  > unary_expression(_r1) > iter_pos)  [ px::bind(&FactorActions::expr_,   _1, _2, _val) ]
                |   ((kw["not"] | '!') > unary_expression(_r1) > iter_pos) [ px::bind(&FactorActions::not_, _1, _val) ]
                |   (kw["min"] > '(' > conditional_expression(_r1) [_val = _1] > ',' > conditional_expression(_r1) > ')')
                                                                    [ px::bind(&expr::min, _val, _2) ]
                |   (kw["max"] > '(' > conditional_expression(_r1) [_val = _1] > ',' > conditional_expression(_r1) > ')')
                                                                    [ px::bind(&expr::max, _val, _2) ]
                |   (kw["random"] > '(' > conditional_expression(_r1) [_val = _1] > ',' > conditional_expression(_r1) > ')')
                                                                    [ px::bind(&MyContext::random, _r1, _val, _2) ]
                |   (kw["filament_change"] > '(' > conditional_expression(_r1) > ')') [ px::bind(&MyContext::filament_change, _r1, _1) ]
                |   (kw["digits"] > '(' > conditional_expression(_r1) [_val = _1] > ',' > conditional_expression(_r1) > optional_parameter(_r1))
                                                                    [ px::bind(&expr::digits<false>, _val, _2, _3) ]
                |   (kw["zdigits"] > '(' > conditional_expression(_r1) [_val = _1] > ',' > conditional_expression(_r1) > optional_parameter(_r1))
                                                                    [ px::bind(&expr::digits<true>, _val, _2, _3) ]
                |   (kw["int"]   > '(' > conditional_expression(_r1) > ')') [ px::bind(&FactorActions::to_int,  _1, _val) ]
                |   (kw["round"] > '(' > conditional_expression(_r1) > ')') [ px::bind(&FactorActions::round,   _1, _val) ]
                |   (kw["ceil"]  > '(' > conditional_expression(_r1) > ')') [ px::bind(&FactorActions::ceil,    _1, _val) ]
                |   (kw["floor"] > '(' > conditional_expression(_r1) > ')') [ px::bind(&FactorActions::floor,   _1, _val) ]
                |   (kw["is_nil"] > '(' > variable_reference(_r1) > ')') [px::bind(&MyContext::is_nil_test, _r1, _1, _val)]
                |   (kw["one_of"] > '(' > one_of(_r1) > ')')        [ _val = _1 ]
                |   (kw["empty"] > '(' > variable_reference(_r1) > ')') [px::bind(&MyContext::is_vector_empty, _r1, _1, _val)]
                |   (kw["size"] > '(' > variable_reference(_r1) > ')') [px::bind(&MyContext::vector_size, _r1, _1, _val)]
                |   (kw["interpolate_table"] > '(' > interpolate_table(_r1) > ')') [ _val = _1 ]
                |   (strict_double > iter_pos)                      [ px::bind(&FactorActions::double_, _r1, _1, _2, _val) ]
                |   (int_      > iter_pos)                          [ px::bind(&FactorActions::int_,    _r1, _1, _2, _val) ]
                |   (kw[bool_] > iter_pos)                          [ px::bind(&FactorActions::bool_,   _r1, _1, _2, _val) ]
                |   raw[lexeme['"' > *((utf8char - char_('\\') - char_('"')) | ('\\' > char_)) > '"']]
                                                                    [ px::bind(&FactorActions::string_, _r1, _1,     _val) ]
                );
            unary_expression.name("unary_expression");

            one_of = (unary_expression(_r1)[_a = _1] > one_of_list(_r1, _a))[_val = _2];
            one_of.name("one_of");
            one_of_list = 
                eps[px::bind(&expr::one_of_test_init, _val)] >
                (   ( ',' > *(
                        (
                                unary_expression(_r1)[px::bind(&expr::one_of_test<false>, _r2, _1, _val)]
                            |   (lit('~') > unary_expression(_r1))[px::bind(&expr::one_of_test<true>, _r2, _1, _val)]
                            |   regular_expression[px::bind(&expr::one_of_test_regex, _r2, _1, _val)]
                        ) >> -lit(','))
                    )
                  | eps
                );
            one_of_list.name("one_of_list");

            interpolate_table = (unary_expression(_r1)[_a = _1] > ',' > interpolate_table_list(_r1, _a))
                [px::bind(&InterpolateTableContext::evaluate, _a, _2, _val)];
            interpolate_table.name("interpolate_table");
            interpolate_table_list =
                eps[px::bind(&InterpolateTableContext::init, _r2)] >
                ( *(( lit('(') > unary_expression(_r1) > ',' > unary_expression(_r1) > ')' ) 
                    [px::bind(&InterpolateTableContext::add_pair, _1, _2, _val)] >> -lit(',')) );
            interpolate_table.name("interpolate_table_list");

            optional_parameter = iter_pos[px::bind(&FactorActions::set_start_pos, _1, _val)] >> (
                    lit(')')                                       [ px::bind(&FactorActions::noexpr, _val) ]
                |   (lit(',') > conditional_expression(_r1) > ')') [ _val = _1 ]
                );
            optional_parameter.name("optional_parameter");

            variable_reference =
                variable(_r1)[_a=_1] >>
                (
                        ('[' > additive_expression(_r1)[px::bind(&MyContext::evaluate_index, _1, _b)] > ']' > iter_pos)
                            [px::bind(&MyContext::store_variable_index, _r1, _a, _b, _2, _val)]
                    |   eps[_val=_a]
                );
            variable_reference.name("variable reference");

            variable = identifier[ px::bind(&MyContext::resolve_variable, _r1, _1, _val) ];
            variable.name("variable name");

            regular_expression = raw[lexeme['/' > *((utf8char - char_('\\') - char_('/')) | ('\\' > char_)) > '/']];
            regular_expression.name("regular_expression");

            keywords.add
                ("and")
                ("digits")
                ("zdigits")
                ("empty")
                ("if")
                ("int")
                ("is_nil")
                ("local")
                //("inf")
                ("else")
                ("elsif")
                ("endif")
                ("false")
                ("global")
                ("interpolate_table")
                ("min")
                ("max")
                ("random")
                ("filament_change")
                ("repeat")
                ("round")
                ("floor")
                ("ceil")
                ("not")
                ("one_of")
                ("or")
                ("size")
                ("true");

            if (0) {
                debug(start);
                debug(text);
                debug(text_block);
                debug(macros);
                debug(if_else_output);
                debug(interpolate_table);
//                debug(switch_output);
                debug(legacy_variable_expansion);
                debug(identifier);
                debug(interpolate_table);
                debug(interpolate_table_list);
                debug(conditional_expression);
                debug(logical_or_expression);
                debug(logical_and_expression);
                debug(equality_expression);
                debug(bool_expr_eval);
                debug(relational_expression);
                debug(additive_expression);
                debug(multiplicative_expression);
                debug(unary_expression);
                debug(one_of);
                debug(one_of_list);
                debug(optional_parameter);
                debug(variable_reference);
                debug(variable);
                debug(regular_expression);
            }
        }

        // Generic expression over expr.
        typedef qi::rule<Iterator, expr(const MyContext*), skipper> RuleExpression;

        // The start of the grammar.
        qi::rule<Iterator, std::string(const MyContext*), qi::locals<bool>, skipper> start;
        // A free-form text.
        qi::rule<Iterator, std::string(), skipper> text;
        // A free-form text, possibly empty, possibly containing macro expansions.
        qi::rule<Iterator, std::string(const MyContext*), skipper> text_block;
        // Statements enclosed in curely braces {}
        qi::rule<Iterator, std::string(const MyContext*), skipper> block, statement, macros, if_text_block, if_macros, else_macros;
        // Legacy variable expansion of the original Slic3r, in the form of [scalar_variable] or [vector_variable_index].
        qi::rule<Iterator, std::string(const MyContext*), skipper> legacy_variable_expansion;
        // Parsed identifier name.
        qi::rule<Iterator, IteratorRange(), skipper> identifier;
        // Ternary operator (?:) over logical_or_expression.
        qi::rule<Iterator, expr(const MyContext*), qi::locals<bool>, skipper> conditional_expression;
        // Logical or over logical_and_expressions.
        RuleExpression logical_or_expression;
        // Logical and over relational_expressions.
        RuleExpression logical_and_expression;
        // <, >, <=, >=
        RuleExpression relational_expression;
        // Math expression consisting of +- operators over multiplicative_expressions.
        RuleExpression additive_expression;
        // Boolean expressions over expressions.
        RuleExpression equality_expression;
        // Math expression consisting of */ operators over factors.
        RuleExpression multiplicative_expression;
        // Number literals, functions, braced expressions, variable references, variable indexing references.
        RuleExpression unary_expression;
        // Accepting an optional parameter.
        RuleExpression optional_parameter;
        // Rule to capture a regular expression enclosed in //.
        qi::rule<Iterator, IteratorRange(), skipper> regular_expression;
        // Evaluate boolean expression into bool.
        qi::rule<Iterator, bool(const MyContext*), skipper> bool_expr_eval;
        // Reference of a scalar variable, or reference to a field of a vector variable.
        qi::rule<Iterator, OptWithPos(const MyContext*), qi::locals<OptWithPos, int>, skipper> variable_reference;
        // Rule to translate an identifier to a ConfigOption, or to fail.
        qi::rule<Iterator, OptWithPos(const MyContext*), skipper> variable;
        // Evaluating whether a nullable variable is nil.
        qi::rule<Iterator, expr(const MyContext*), skipper> is_nil_test;
        // Evaluating "one of" list of patterns.
        qi::rule<Iterator, expr(const MyContext*), qi::locals<expr>, skipper> one_of;
        qi::rule<Iterator, expr(const MyContext*, const expr &param), skipper> one_of_list;
        // Evaluating the "interpolate_table" expression.
        qi::rule<Iterator, expr(const MyContext*), qi::locals<expr>, skipper> interpolate_table;
        qi::rule<Iterator, InterpolateTableContext(const MyContext*, const expr &param), skipper> interpolate_table_list;

        qi::rule<Iterator, std::string(const MyContext*), qi::locals<bool>, skipper> if_else_output;
        qi::rule<Iterator, std::string(const MyContext*), qi::locals<OptWithPos>, skipper> assignment_statement;
        // Allocating new local or global variables.
        qi::rule<Iterator, std::string(const MyContext*), qi::locals<bool, MyContext::NewOldVariable>, skipper> new_variable_statement;
        qi::rule<Iterator, std::vector<expr>(const MyContext*), skipper> initializer_list;

        qi::symbols<char> keywords;
    };
}

static const client::macro_processor g_macro_processor_instance;

static std::string process_macro(const std::string &templ, client::MyContext &context)
{
    std::string output;
    phrase_parse(templ.begin(), templ.end(), g_macro_processor_instance(&context), client::skipper{}, output);
	if (! context.error_message.empty()) {
        if (context.error_message.back() != '\n' && context.error_message.back() != '\r')
            context.error_message += '\n';
        throw Slic3r::PlaceholderParserError(context.error_message);
    }
    return output;
}

std::string PlaceholderParser::process(const std::string &templ, unsigned int current_extruder_id, const DynamicConfig *config_override, DynamicConfig *config_outputs, ContextData *context_data) const
{
    client::MyContext context;
    context.external_config 	= this->external_config();
    context.config              = &this->config();
    context.config_override     = config_override;
    context.config_outputs      = config_outputs;
    context.current_extruder_id = current_extruder_id;
    context.context_data        = context_data;
    return process_macro(templ, context);
}

// Evaluate a boolean expression using the full expressive power of the PlaceholderParser boolean expression syntax.
// Throws Slic3r::RuntimeError on syntax or runtime error.
bool PlaceholderParser::evaluate_boolean_expression(const std::string &templ, const DynamicConfig &config, const DynamicConfig *config_override)
{
    client::MyContext context;
    context.config              = &config;
    context.config_override     = config_override;
    // Let the macro processor parse just a boolean expression, not the full macro language.
    context.just_boolean_expression = true;
    return process_macro(templ, context) == "true";
}

}
