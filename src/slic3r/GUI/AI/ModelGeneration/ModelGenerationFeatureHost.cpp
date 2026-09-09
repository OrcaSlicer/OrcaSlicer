#include "ModelGenerationFeatureHost.hpp"

#include "slic3r/GUI/AI/Orca/OrcaWorkspaceAdapter.hpp"
#include "slic3r/GUI/ModelGenerationPanel.hpp"

#include <boost/log/trivial.hpp>
#include <wx/colour.h>
#include <wx/window.h>

#include <utility>

namespace Slic3r::GUI {

struct ModelGenerationFeatureHost::Impl
{
    Impl(wxWindow* parent, Plater* plater, NavigateAfterImportFn navigate_after_import, RetryServiceFn retry_service)
        : workspace(std::make_unique<OrcaWorkspaceAdapter>(plater, navigate_after_import))
    {
        BOOST_LOG_TRIVIAL(info) << "AI model generation startup: creating model generation panel";
        model_generation = new ModelGenerationPanel(parent, *workspace, *workspace);
        model_generation->set_service_retry_handler(std::move(retry_service));
        model_generation->set_prepare_navigation_handler(std::move(navigate_after_import));
        model_generation->Hide();
        BOOST_LOG_TRIVIAL(info) << "AI model generation startup: model generation panel created";
    }

    void shutdown()
    {
        if (shutdown_requested)
            return;
        shutdown_requested = true;
        if (model_generation != nullptr) {
            model_generation->set_service_retry_handler({});
            model_generation->set_prepare_navigation_handler({});
            model_generation->shutdown();
        }
    }

    std::unique_ptr<OrcaWorkspaceAdapter> workspace;
    ModelGenerationPanel* model_generation { nullptr };
    bool shutdown_requested { false };
};

ModelGenerationFeatureHost::ModelGenerationFeatureHost(wxWindow* parent, Plater* plater,
                                                       NavigateAfterImportFn navigate_after_import,
                                                       RetryServiceFn retry_service)
    : m_impl(std::make_unique<Impl>(parent, plater, std::move(navigate_after_import), std::move(retry_service)))
{}

ModelGenerationFeatureHost::~ModelGenerationFeatureHost()
{
    shutdown();
}

wxWindow* ModelGenerationFeatureHost::panel() const
{
    return m_impl->model_generation;
}

void ModelGenerationFeatureHost::set_service_availability(bool available, const std::string& message)
{
    if (m_impl->model_generation != nullptr)
        m_impl->model_generation->set_service_availability(available, message);
}

void ModelGenerationFeatureHost::shutdown()
{
    m_impl->shutdown();
}

} // namespace Slic3r::GUI
