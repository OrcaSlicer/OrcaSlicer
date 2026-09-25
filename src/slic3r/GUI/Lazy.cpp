#include "Lazy.hpp"

#include <boost/log/trivial.hpp>
#include <wx/app.h>
#include <wx/utils.h>

namespace Slic3r { namespace GUI {

LazyBase::OnDemandBuild::OnDemandBuild(const LazyBase& lazy) : m_lazy(lazy), m_started(std::chrono::steady_clock::now())
{
    // The unit tests have no app.
    if (wxTheApp != nullptr)
        m_busy = std::make_unique<wxBusyCursor>();
}

LazyBase::OnDemandBuild::~OnDemandBuild()
{
    BOOST_LOG_TRIVIAL(info) << "Lazy::ensure: built " << m_lazy.name() << " on demand, " << m_units << " unit(s) in "
                            << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_started).count() << " ms";
}

void LazyBase::log_null_factory(const std::string& name)
{
    BOOST_LOG_TRIVIAL(error) << "Lazy::build_step: the factory for " << name << " returned null";
}

}} // namespace Slic3r::GUI
