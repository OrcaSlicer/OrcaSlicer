#ifndef ORCASLICER_CACHESINK_HPP
#define ORCASLICER_CACHESINK_HPP
#include <boost/log/sinks/basic_sink_backend.hpp>
#include <boost/log/sinks/basic_sink_frontend.hpp>
#include <boost/log/sinks/frontend_requirements.hpp>
#include <boost/phoenix/operator/comparison.hpp>

namespace Slic3r {
namespace logging = boost::log;
namespace sinks = logging::sinks;

class CacheSink : public sinks::basic_sink_backend<sinks::synchronized_feeding>
{
    std::vector<logging::record_view> m_cached_records;
    logging::filter m_filter;

    public:
    void consume(logging::record_view const& rec)
    {
        m_cached_records.push_back(rec);
    }

    void set_log_level(logging::trivial::severity_level level)
    {
        m_filter = boost::log::trivial::severity >= level;
    }

    void forward_records(sinks::basic_sink_frontend& sink) const
    {
        for (auto& rec : m_cached_records)
            if (m_filter(rec.attribute_values()))
                sink.consume(rec);
    }
};

} // namespace slic3r

#endif // ORCASLICER_CACHESINK_HPP
