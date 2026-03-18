#include "NotificationProgressIndicator.hpp"
#include "slic3r/GUI/NotificationManager.hpp"

namespace Slic3r { namespace GUI {

NotificationProgressIndicator::NotificationProgressIndicator(NotificationManager *nm): m_nm{nm} {}

void NotificationProgressIndicator::clear_percent()
{

}

void NotificationProgressIndicator::show_error_info(const std::string &msg, int code, const std::string &description, const std::string &extra)
{

}

void NotificationProgressIndicator::set_range(int range)
{
    m_nm->progress_indicator_set_range(range);
}

void NotificationProgressIndicator::set_cancel_callback(CancelFn fn)
{
    m_cancelfn = std::move(fn);
    m_nm->progress_indicator_set_cancel_callback(m_cancelfn);
}

void NotificationProgressIndicator::set_progress(int pr)
{
    if (!pr)
        set_cancel_callback(m_cancelfn);

    m_nm->progress_indicator_set_progress(pr);
}

void NotificationProgressIndicator::set_status_text(const char *msg)
{
    m_nm->progress_indicator_set_status_text(msg);
}

int NotificationProgressIndicator::get_range() const
{
    return m_nm->progress_indicator_get_range();
}

}} // namespace Slic3r::GUI
