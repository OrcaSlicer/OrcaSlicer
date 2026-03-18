#ifndef ORCASLICER_LOGGINGEXTENSIONS_HPP
#define ORCASLICER_LOGGINGEXTENSIONS_HPP
#include <boost/log/expressions/keyword.hpp>
#include <boost/log/attributes/mutable_constant.hpp>
#include <boost/log/sources/global_logger_storage.hpp>

namespace Slic3r {
BOOST_LOG_ATTRIBUTE_KEYWORD_IMPL(function_name, "FunctionName", std::string, logging_tags)
BOOST_LOG_ATTRIBUTE_KEYWORD_IMPL(line_number, "LineNumber", int, logging_tags)
BOOST_LOG_ATTRIBUTE_KEYWORD_IMPL(file_name, "FileName", std::string, logging_tags)

namespace logging_keywords {
BOOST_PARAMETER_KEYWORD(Slic3r::logging_tag, function_name)
BOOST_PARAMETER_KEYWORD(Slic3r::logging_tag, line_number)
BOOST_PARAMETER_KEYWORD(Slic3r::logging_tag, file_name)
}

namespace trivial {
using namespace boost::log::sources;
namespace trivial = boost::log::trivial;

template<typename BaseT>
class function_info_feature : public BaseT
{
public:
    typedef typename BaseT::char_type char_type;
    typedef typename BaseT::threading_model threading_model;

    typedef boost::log::attributes::mutable_constant<std::string> function_name_attribute;
    typedef boost::log::attributes::mutable_constant<int> line_number_attribute;
    typedef boost::log::attributes::mutable_constant<std::string> file_name_attribute;

    typedef typename boost::log::strictest_lock<
        boost::log::no_lock< threading_model >,
        typename BaseT::open_record_lock,
        typename BaseT::add_attribute_lock
    >::type open_record_lock;

    function_info_feature() :
    m_FunctionNameAttr(""),
    m_LineNumberAttr(0),
    m_FileNameAttr("")
    {
        add_attributes();
    }

    function_info_feature(function_info_feature const& that) :
    BaseT(static_cast<BaseT const&>(that)),
    m_FunctionNameAttr(that.m_FunctionNameAttr),
    m_LineNumberAttr(that.m_LineNumberAttr),
    m_FileNameAttr(that.m_FileNameAttr)
    {
        auto attrs = BaseT::attributes();
        attrs[logging_tags::function_name::get_name()] = m_FunctionNameAttr;
        attrs[logging_tags::line_number::get_name()] = m_LineNumberAttr;
        attrs[logging_tags::file_name::get_name()] = m_FileNameAttr;
    }

    function_info_feature(function_info_feature&& that) BOOST_NOEXCEPT_IF(boost::is_nothrow_move_constructible< BaseT >::value &&
                                                                                         boost::is_nothrow_move_constructible< function_name_attribute >::value &&
                                                                                         boost::is_nothrow_move_constructible< line_number_attribute >::value &&
                                                                                         boost::is_nothrow_move_constructible< file_name_attribute >::value ) :
    BaseT(boost::move(static_cast< BaseT& >(that))),
    m_FunctionNameAttr(boost::move(that.m_FunctionNameAttr)),
    m_LineNumberAttr(boost::move(that.m_LineNumberAttr)),
    m_FileNameAttr(boost::move(that.m_FileNameAttr))
    {}

    template<typename ArgsT>
    explicit function_info_feature(ArgsT const& args) :
    BaseT(args),
    m_FunctionNameAttr(args[logging_keywords::function_name | std::string()]),
    m_LineNumberAttr(args[logging_keywords::line_number | -1]),
    m_FileNameAttr(args[logging_keywords::file_name | std::string()])
    {
        add_attributes();
    }

private:
    function_name_attribute m_FunctionNameAttr;
    line_number_attribute m_LineNumberAttr;
    file_name_attribute m_FileNameAttr;

protected:
    template<typename ArgsT>
    boost::log::record open_record_unlocked(ArgsT const& args)
    {
        m_FunctionNameAttr.set(args[logging_keywords::function_name | std::string()]);
        m_LineNumberAttr.set(args[logging_keywords::line_number | -1]);
        m_FileNameAttr.set(args[logging_keywords::file_name | std::string()]);

        return BaseT::open_record_unlocked(args);
    }

    void add_attributes()
    {
        BaseT::add_attribute_unlocked(logging_tags::function_name::get_name(), m_FunctionNameAttr);
        BaseT::add_attribute_unlocked(logging_tags::line_number::get_name(), m_LineNumberAttr);
        BaseT::add_attribute_unlocked(logging_tags::file_name::get_name(), m_FileNameAttr);
    }
};

struct function_info : boost::mpl::quote1<function_info_feature>{};

class severity_and_function_info_logger_mt : public basic_composite_logger<
        char,
        severity_and_function_info_logger_mt,
        multi_thread_model< boost::log::aux::light_rw_mutex >,
        features< severity< trivial::severity_level >, function_info >
>
{
    BOOST_LOG_FORWARD_LOGGER_MEMBERS_TEMPLATE(severity_and_function_info_logger_mt)
};

struct logger
{
    typedef severity_and_function_info_logger_mt logger_type;

    static logger_type& get()
    {
        return aux::logger_singleton< logger >::get();
    }

    enum registration_line_t { registration_line = __LINE__ };
    static const char* registration_file() { return __FILE__; }
    static logger_type construct_logger()
    {
        return logger_type(boost::log::keywords::severity = trivial::info);
    }
};
}
}

#endif // ORCASLICER_LOGGINGEXTENSIONS_HPP
