#ifndef slic3r_BooleanOperationJob_hpp_
#define slic3r_BooleanOperationJob_hpp_

#include <atomic>
#include <memory>
#include <string>
#include <functional>
#include <vector>

#include "slic3r/GUI/Camera.hpp"
#include "JobNew.hpp"
#include "libslic3r/TriangleMesh.hpp"

namespace Slic3r {
    namespace GUI {
        enum EBooleanOperationState
        {
            NotStart,
            Running,
            Finished,
            Cancelling,
            Canceled,
            Failed
        };
        template<class JobData>
        class BooleanOperationJob : public JobNew
        {
        public:
            using BoolearOperationCancelCallback = std::function<bool(JobNew::Ctl& ctl, JobData&)>;
            using BoolearOperationProcessCallback = std::function<void(JobNew::Ctl& ctl, JobData&)>;
            using BoolearOperationCallback = std::function<void(JobData&)>;
            using BoolearOperationProgressCallback = std::function<void(float progress)>;
            using BoolearOperationFailedCallback = std::function<void(JobData&)>;
            explicit BooleanOperationJob()
            {

            }
            ~BooleanOperationJob()
            {

            }

            void set_process_callback(const BoolearOperationProcessCallback& cb)
            {
                m_process_cb = cb;
            }

            void set_finalize_callback(const BoolearOperationCallback& cb)
            {
                m_finalize_cb = cb;
            }

            void set_cancel_callback(const BoolearOperationCancelCallback& cb)
            {
                m_cancel_cb = cb;
            }

            void set_progress_callback(const BoolearOperationProgressCallback& cb)
            {
                m_progress_cb = cb;
            }

            void set_failed_callback(const BoolearOperationFailedCallback& cb)
            {
                m_failed_cb = cb;
            }

            void set_data(JobData& data)
            {
                m_data = data;
            }

            void process(Ctl& ctl) override
            {
                if (m_process_cb) {
                    if (m_cancel_cb) {
                        m_data.cancel_cb = [&]()->bool {
                            return m_cancel_cb(ctl, m_data);
                        };
                    }
                    if (m_failed_cb) {
                        m_data.failed_cb = [&]()->void {
                            m_failed_cb(m_data);
                        };
                    }
                    m_data.progress_cb = m_progress_cb;
                    m_process_cb(ctl, m_data);
                }
            }

            void finalize(bool canceled, std::exception_ptr& eptr) override
            {
                if (m_finalize_cb) {
                    if (eptr) {
                        m_data.state = EBooleanOperationState::Failed;
                    }
                    m_finalize_cb(m_data);
                }
            }

            bool show_busy_cursor() override
            {
                return false;
            };

        private:
            BoolearOperationProcessCallback m_process_cb{ nullptr };
            BoolearOperationCallback m_finalize_cb{ nullptr };
            BoolearOperationCancelCallback m_cancel_cb{ nullptr };
            BoolearOperationProgressCallback m_progress_cb{ nullptr };
            BoolearOperationFailedCallback m_failed_cb{ nullptr };

            JobData m_data;
        };
    } // namespace Slic3r::GUI
} // namespace Slic3r

#endif // slic3r_BooleanOperationJob_hpp_
