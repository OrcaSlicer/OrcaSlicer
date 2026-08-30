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
struct GCodeProcessorResult;
namespace GUI { struct PreviewTriangleMesh; }

class PluginVisualizations
{
public:
    using ChangeToken = size_t;
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

    // The preview-scene producer is one adapter for the generic visualization contract. Other producers can
    // negotiate an input and call open/update directly without depending on preview meshes.
    void request_open_toolpath(const std::shared_ptr<VisualizationPluginCapability>& capability,
                               std::shared_ptr<const GUI::PreviewTriangleMesh> mesh,
                               const GCodeProcessorResult& result, const Pointfs& printable_area, int plate_index, uint64_t revision,
                               std::string orca_version, Completion completion = {});
    void request_toolpath_updates(std::shared_ptr<const GUI::PreviewTriangleMesh> mesh,
                                  const GCodeProcessorResult& result, const Pointfs& printable_area, int plate_index, uint64_t revision,
                                  std::string orca_version, Completion completion = {});

    ChangeToken subscribe(std::function<void()> callback);
    void        unsubscribe(ChangeToken token);

private:
    enum class RequestKind { Open, Update };

    struct Session
    {
        std::shared_ptr<VisualizationPluginCapability> capability;
        uint64_t                                       revision{0};
        boost::filesystem::path                        snapshot_path;
        VisualizationInput                             input;
    };

    struct Request
    {
        RequestKind                                    kind{RequestKind::Open};
        std::shared_ptr<VisualizationPluginCapability> capability;
        PluginCapabilityId                             id;
        std::shared_ptr<const PreviewGeometrySnapshot::Snapshot> snapshot;
        int                                            plate_index{-1};
        uint64_t                                       revision{0};
        uint64_t                                       generation{0};
        std::string                                    orca_version;
        boost::filesystem::path                        snapshot_path;
        VisualizationInput                             input;
        Completion                                     completion;
    };

    PluginVisualizations() = default;
    PluginVisualizations(const PluginVisualizations&) = delete;
    PluginVisualizations& operator=(const PluginVisualizations&) = delete;

    void enqueue(Request request);
    void ensure_worker_locked();
    void worker_loop();
    void dispatch_request(Request request);
    bool is_current_locked(const Request& request) const;
    void finish_request(const Request& request, ExecutionResult result);
    boost::filesystem::path prepare_cache(const std::string& plugin_key, int plate_index, uint64_t revision,
                                          const std::string& extension = ".glb");
    void remove_snapshot(const boost::filesystem::path& path);
    void notify_change();

    mutable std::mutex                           m_mutex;
    std::recursive_mutex                         m_dispatch_mutex;
    std::condition_variable                      m_worker_cv;
    std::map<PluginCapabilityId, Session>        m_sessions;
    std::map<PluginCapabilityId, uint64_t>       m_generations;
    std::deque<Request>                          m_requests;
    std::thread                                  m_worker;
    bool                                         m_stop_worker{false};
    std::set<std::string>                        m_initialized_cache_plugins;
    std::map<ChangeToken, std::function<void()>> m_callbacks;
    ChangeToken                                  m_next_token{1};
};

} // namespace Slic3r
