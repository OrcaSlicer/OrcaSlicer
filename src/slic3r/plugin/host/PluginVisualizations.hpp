#pragma once

#include "../pluginTypes/visualization/VisualizationPluginCapability.hpp"
#include "PreviewGeometrySnapshot.hpp"

#include <boost/filesystem/path.hpp>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace Slic3r {

class PluginManager;
class TriangleMesh;

class PluginVisualizations
{
public:
    using Completion = std::function<void(ExecutionResult)>;

    static PluginVisualizations& instance();
    ~PluginVisualizations();

    void install_callbacks(PluginManager& manager);

    std::vector<std::shared_ptr<VisualizationPluginCapability>> capabilities() const;
    std::vector<std::shared_ptr<VisualizationPluginCapability>> capabilities_for(const VisualizationInput& input) const;

    ExecutionResult open(const std::shared_ptr<VisualizationPluginCapability>& capability, const VisualizationContext& ctx,
                         boost::filesystem::path owned_resource = {});
    ExecutionResult update(const PluginCapabilityId& id, const VisualizationContext& ctx,
                           boost::filesystem::path owned_resource = {});
    void            close(const PluginCapabilityId& id);
    void            close_plugin(const std::string& plugin_key);
    void            close_all();
    bool            is_active(const PluginCapabilityId& id) const;
    bool            has_active_sessions() const;

    void request_open(const std::shared_ptr<VisualizationPluginCapability>& capability,
                      const std::vector<VisualizationResourceRequest>& resources = {}, Completion completion = {});
    bool request_update(const PluginCapabilityId& id,
                        const std::vector<VisualizationResourceRequest>& resources = {});

private:
    enum class RequestKind { Open, Update };

    struct Session
    {
        std::shared_ptr<VisualizationPluginCapability> capability;
        uint64_t                                       revision{0};
        std::vector<boost::filesystem::path>           snapshot_paths;
        std::vector<VisualizationInput>                inputs;
        std::vector<VisualizationResourceRequest>      resource_requests;
    };

    struct PendingResource
    {
        VisualizationInput                                      input;
        int                                                     plate_index{-1};
        boost::filesystem::path                                 snapshot_path;
        std::shared_ptr<const PreviewGeometrySnapshot::Snapshot> toolpath;
        std::shared_ptr<const TriangleMesh>                      model;
    };

    struct Request
    {
        RequestKind                                    kind{RequestKind::Open};
        std::shared_ptr<VisualizationPluginCapability> capability;
        PluginCapabilityId                             id;
        uint64_t                                       revision{0};
        uint64_t                                       generation{0};
        std::string                                    orca_version;
        std::vector<VisualizationResourceRequest>      resource_requests;
        std::vector<PendingResource>                   resources;
        Completion                                     completion;
    };

    PluginVisualizations() = default;
    PluginVisualizations(const PluginVisualizations&) = delete;
    PluginVisualizations& operator=(const PluginVisualizations&) = delete;

    void capture_and_enqueue(const std::shared_ptr<VisualizationPluginCapability>& capability,
                             const std::vector<VisualizationResourceRequest>& resources,
                             RequestKind kind, Completion completion = {});
    void enqueue(Request request);
    void ensure_worker_locked();
    void worker_loop();
    void dispatch_request(Request request);
    bool is_current_locked(const Request& request) const;
    void finish_request(const Request& request, ExecutionResult result);
    boost::filesystem::path prepare_cache(const std::string& plugin_key, int plate_index, uint64_t revision,
                                          const std::string& extension = ".glb");
    ExecutionResult open_resources(const std::shared_ptr<VisualizationPluginCapability>& capability,
                                   const VisualizationContext& ctx,
                                   std::vector<boost::filesystem::path> owned_resources,
                                   std::vector<VisualizationResourceRequest> resource_requests);
    ExecutionResult update_resources(const PluginCapabilityId& id, const VisualizationContext& ctx,
                                     std::vector<boost::filesystem::path> owned_resources,
                                     std::vector<VisualizationResourceRequest> resource_requests);
    void remove_snapshot(const boost::filesystem::path& path);
    void remove_snapshots(const std::vector<boost::filesystem::path>& paths);

    mutable std::mutex                           m_mutex;
    std::recursive_mutex                         m_dispatch_mutex;
    std::condition_variable                      m_worker_cv;
    std::map<PluginCapabilityId, Session>        m_sessions;
    std::map<PluginCapabilityId, uint64_t>       m_generations;
    std::deque<Request>                          m_requests;
    std::thread                                  m_worker;
    bool                                         m_stop_worker{false};
    std::set<std::string>                        m_initialized_cache_plugins;
    uint64_t                                     m_next_revision{1};
};

} // namespace Slic3r
