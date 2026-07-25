#include "DevStatus.h"
#include "slic3r/GUI/DeviceManager.hpp"
#include <boost/log/trivial.hpp>

namespace Slic3r {

void DevStatus::ParseStatus(const nlohmann::json& print_jj)
{
    try {
        if (print_jj.contains("job")) {
            const nlohmann::json& job_jj = print_jj.at("job");
            if (job_jj.contains("job_state")) {
                auto new_state = (DevJobState)job_jj.at("job_state").get<int>();
                if (m_job_state != new_state) {
                    m_job_state = new_state;
                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": job_state-> " << (int)new_state;
                }

                if (m_job_state == DevJobState::JobStateFinishing) {
                    m_job_state = DevJobState::JobStateFinish;
                }
            }
        }
    } catch (const std::exception& e) {
#if BBL_RELEASE_TO_PUBLIC
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": get exception";
#else
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": get exception=" << e.what();
#endif
    }
}

} // namespace Slic3r