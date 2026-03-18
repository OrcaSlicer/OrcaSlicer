#ifndef IPROGRESSINDICATOR_HPP
#define IPROGRESSINDICATOR_HPP

#include <string>
#include <functional>

namespace Slic3r {

/**
 * @brief Generic progress indication interface.
 */
class ProgressIndicator {
public:
    
    /// Cancel callback function type
    using CancelFn = std::function<void()>;
    
    virtual ~ProgressIndicator() = default;
    
    virtual void clear_percent() = 0;
    virtual void show_error_info(const std::string &msg, int code, const std::string &description, const std::string &extra) = 0;
    virtual void set_range(int range) = 0;
    virtual void set_cancel_callback(CancelFn = CancelFn()) = 0;
    virtual void set_progress(int pr) = 0;
    virtual void set_status_text(const char *) = 0; // utf8 char array
    virtual int  get_range() const = 0;
};

}

#endif // IPROGRESSINDICATOR_HPP
