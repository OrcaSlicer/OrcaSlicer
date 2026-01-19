#ifndef libslic3r_Timer_hpp_
#define libslic3r_Timer_hpp_

#include <string>
#include <chrono>

namespace Slic3r {

/// <summary>
/// Instance of this class is used for measure time consumtion
/// of block code until instance is alive and write result to debug output
/// </summary>
class Timer
{
    std::string m_name;
    std::chrono::steady_clock::time_point m_start;
public:
    /// <summary>
    /// name describe timer
    /// </summary>
    /// <param name="name">Describe timer in consol log</param>
    Timer(const std::string& name);

    /// <summary>
    /// name describe timer
    /// </summary>
    ~Timer();
};

namespace Timing {

    // Timing code from Catch2 unit testing library
    static inline uint64_t nanoseconds_since_epoch() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now().time_since_epoch()).count();
    }

    // Timing code from Catch2 unit testing library
    class Timer {
    public:
        void start() {
            m_nanoseconds = nanoseconds_since_epoch();
        }
        uint64_t elapsed_nanoseconds() const {
            return nanoseconds_since_epoch() - m_nanoseconds;
        }
        uint64_t elapsed_microseconds() const {
            return elapsed_nanoseconds() / 1000;
        }
        unsigned int elapsed_milliseconds() const {
            return static_cast<unsigned int>(elapsed_microseconds()/1000);
        }
        double elapsed_seconds() const {
            return elapsed_microseconds() / 1000000.0;
        }
    private:
        uint64_t m_nanoseconds = 0;
    };

    // Emits a Boost::log error if the life time of this timing object exceeds a limit.
    class TimeLimitAlarm {
    public:
        TimeLimitAlarm(uint64_t time_limit_nanoseconds, std::string_view limit_exceeded_message) :
            m_time_limit_nanoseconds(time_limit_nanoseconds), m_limit_exceeded_message(limit_exceeded_message) { 
            m_timer.start();
        }
        ~TimeLimitAlarm() {
            auto elapsed = m_timer.elapsed_nanoseconds();
            if (elapsed > m_time_limit_nanoseconds)
                this->report_time_exceeded();
        }
        static TimeLimitAlarm new_nanos(uint64_t time_limit_nanoseconds, std::string_view limit_exceeded_message) {
            return TimeLimitAlarm(time_limit_nanoseconds, limit_exceeded_message);
        }
        static TimeLimitAlarm new_milis(uint64_t time_limit_milis, std::string_view limit_exceeded_message) {
            return TimeLimitAlarm(uint64_t(time_limit_milis) * 1000000l, limit_exceeded_message);
        }
        static TimeLimitAlarm new_seconds(uint64_t time_limit_seconds, std::string_view limit_exceeded_message) {
            return TimeLimitAlarm(uint64_t(time_limit_seconds) * 1000000000l, limit_exceeded_message);
        }
    private:
        void report_time_exceeded() const;

        Timer               m_timer;
        uint64_t            m_time_limit_nanoseconds;
        std::string_view    m_limit_exceeded_message;
    };

} // namespace Catch

} // namespace Slic3r

#endif // libslic3r_Timer_hpp_
