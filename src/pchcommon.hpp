#pragma once

// This header is included in every build, even those with SLIC3R_PCH disabled

#ifdef  __cplusplus

#include <boost/log/trivial.hpp>
#include <libslic3r/Logging/LoggingExtensions.hpp>

#undef BOOST_LOG_TRIVIAL
#define BOOST_LOG_TRIVIAL(lvl)\
BOOST_LOG_STREAM_WITH_PARAMS(::Slic3r::trivial::logger::get(),\
(::boost::log::keywords::severity = ::boost::log::trivial::lvl)\
(::Slic3r::logging_keywords::function_name = __FUNCTION__)\
(::Slic3r::logging_keywords::line_number = __LINE__)\
(::Slic3r::logging_keywords::file_name = __FILE__)\
)

#endif // __cplusplus
