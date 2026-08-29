#include "PluginVisualizations.hpp"

#include "PreviewGeometrySnapshot.hpp"
#include "../../GUI/ToolpathMeshBuilder.hpp"
#include "../PluginManager.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <sstream>
#include <utility>

#include <wx/app.h>
#include <wx/thread.h>

namespace Slic3r {
namespace {

ExecutionResult recoverable_error(const PluginCapabilityId& id, const char* operation, const std::exception& error)
{
    BOOST_LOG_TRIVIAL(error) << "Visualization capability " << id.plugin_key << "/" << id.name << " failed to " << operation << ": "
                             << error.what();
    return ExecutionResult::failure(PluginResult::RecoverableError, error.what());
}

std::string metadata_json(uint64_t scene_id, int plate_index)
{
    std::ostringstream json;
    json << "{\"format\":\"ORPV\",\"major_version\":" << PreviewGeometrySnapshot::FORMAT_MAJOR
         << ",\"minor_version\":" << PreviewGeometrySnapshot::FORMAT_MINOR
         << ",\"scene_id\":" << scene_id << ",\"plate_index\":" << plate_index << "}";
    return json.str();
}

} // namespace

PluginVisualizations& PluginVisualizations::instance()
{
    static PluginVisualizations visualizations;
    return visualizations;
}

PluginVisualizations::~PluginVisualizations()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop_worker = true;
        m_requests.clear();
        for (auto& generation : m_generations)
            ++generation.second;
    }
    m_worker_cv.notify_all();
    if (m_worker.joinable())
        m_worker.join();
}

void PluginVisualizations::install_callbacks(PluginManager& manager)
{
    manager.subscribe_on_capability_load_callback([this](const PluginCapabilityId& id) {
        if (id.type == PluginCapabilityType::Visualization) {
            try {
                prepare_cache(id.plugin_key, 0, 0);
            } catch (const std::exception& error) {
                BOOST_LOG_TRIVIAL(warning) << "Unable to initialize visualization cache for " << id.plugin_key << ": " << error.what();
            }
            notify_change();
        }
    });
    manager.subscribe_on_capability_unload_callback([this](const PluginCapabilityId& id) {
        if (id.type == PluginCapabilityType::Visualization) {
            close(id);
            notify_change();
        }
    });
    manager.subscribe_on_unload_callback([this](const std::string& plugin_key) {
        close_plugin(plugin_key);
        notify_change();
    });
}

std::vector<std::shared_ptr<VisualizationPluginCapability>> PluginVisualizations::capabilities() const
{
    std::vector<std::shared_ptr<VisualizationPluginCapability>> result;
    for (const auto& capability : PluginManager::instance().get_plugin_capabilities("", PluginCapabilityType::Visualization))
        if (auto visualization = std::dynamic_pointer_cast<VisualizationPluginCapability>(capability))
            result.push_back(std::move(visualization));
    return result;
}

ExecutionResult PluginVisualizations::open(const std::shared_ptr<VisualizationPluginCapability>& capability, VisualizationContext& ctx)
{
    if (!capability || !capability->is_enabled())
        return ExecutionResult::skipped("Visualization capability is unavailable");

    const PluginCapabilityId id = capability->identity();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_sessions.count(id) != 0)
            return ExecutionResult::skipped("Visualization session is already open");
    }

    ExecutionResult result;
    try {
        std::lock_guard<std::recursive_mutex> dispatch_lock(m_dispatch_mutex);
        PluginCapabilityInterface::RefCounter ref_counter(*capability);
        result = capability->open(ctx);
    } catch (const std::exception& error) {
        return recoverable_error(id, "open", error);
    }

    if (result.status == PluginResult::Success) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_sessions.emplace(id, Session{capability, ctx.scene_id, ctx.geometry_path});
    }
    return result;
}

ExecutionResult PluginVisualizations::update(const PluginCapabilityId& id, VisualizationContext& ctx)
{
    std::shared_ptr<VisualizationPluginCapability> capability;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_sessions.find(id);
        if (it == m_sessions.end())
            return ExecutionResult::skipped("Visualization session is not open");
        if (it->second.scene_id == ctx.scene_id)
            return ExecutionResult::skipped("Visualization scene is unchanged");
        capability = it->second.capability;
    }

    ExecutionResult result;
    try {
        std::lock_guard<std::recursive_mutex> dispatch_lock(m_dispatch_mutex);
        PluginCapabilityInterface::RefCounter ref_counter(*capability);
        result = capability->update(ctx);
    } catch (const std::exception& error) {
        return recoverable_error(id, "update", error);
    }

    boost::filesystem::path old_snapshot;
    if (result.status == PluginResult::Success) {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_sessions.find(id);
        if (it != m_sessions.end() && it->second.capability == capability) {
            old_snapshot = std::move(it->second.snapshot_path);
            it->second.scene_id = ctx.scene_id;
            it->second.snapshot_path = ctx.geometry_path;
        }
    }
    if (!old_snapshot.empty() && old_snapshot != ctx.geometry_path)
        remove_snapshot(old_snapshot);
    return result;
}

void PluginVisualizations::close(const PluginCapabilityId& id)
{
    std::shared_ptr<VisualizationPluginCapability> capability;
    boost::filesystem::path snapshot_path;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_generations[id];
        m_requests.erase(std::remove_if(m_requests.begin(), m_requests.end(), [&id](const Request& request) { return request.id == id; }),
                         m_requests.end());
        const auto it = m_sessions.find(id);
        if (it == m_sessions.end())
            return;
        capability = std::move(it->second.capability);
        snapshot_path = std::move(it->second.snapshot_path);
        m_sessions.erase(it);
    }

    try {
        std::lock_guard<std::recursive_mutex> dispatch_lock(m_dispatch_mutex);
        PluginCapabilityInterface::RefCounter ref_counter(*capability);
        capability->close();
    } catch (const std::exception& error) {
        recoverable_error(id, "close", error);
    }
    remove_snapshot(snapshot_path);
}

void PluginVisualizations::close_plugin(const std::string& plugin_key)
{
    std::vector<PluginCapabilityId> sessions;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& entry : m_sessions)
            if (entry.first.plugin_key == plugin_key)
                sessions.push_back(entry.first);
        for (auto& generation : m_generations)
            if (generation.first.plugin_key == plugin_key)
                ++generation.second;
        m_requests.erase(std::remove_if(m_requests.begin(), m_requests.end(), [&plugin_key](const Request& request) {
            return request.id.plugin_key == plugin_key;
        }), m_requests.end());
    }
    for (const PluginCapabilityId& id : sessions)
        close(id);

    try {
        const boost::filesystem::path cache = boost::filesystem::path(PluginManager::instance().get_storage_dir(plugin_key)) / "preview_cache";
        boost::system::error_code ignored;
        boost::filesystem::remove_all(cache, ignored);
    } catch (...) {
        // Plugin rows can already be gone during teardown; individual snapshot cleanup above is sufficient.
    }
}

void PluginVisualizations::close_all()
{
    std::vector<PluginCapabilityId> sessions;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& entry : m_sessions)
            sessions.push_back(entry.first);
        for (auto& generation : m_generations)
            ++generation.second;
        m_requests.clear();
    }
    for (const PluginCapabilityId& id : sessions)
        close(id);
}

bool PluginVisualizations::is_active(const PluginCapabilityId& id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_sessions.count(id) != 0;
}

bool PluginVisualizations::has_active_sessions() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return !m_sessions.empty();
}

void PluginVisualizations::request_open(const std::shared_ptr<VisualizationPluginCapability>& capability,
                                        std::shared_ptr<const GUI::PreviewTriangleMesh> mesh,
                                        const GCodeProcessorResult& result, const Pointfs& printable_area, int plate_index, uint64_t scene_id,
                                        std::string orca_version, Completion completion)
{
    if (!capability || !capability->is_enabled() || !mesh) {
        if (completion)
            completion(ExecutionResult::skipped(mesh ? "Visualization capability is unavailable" : "Preview mesh is unavailable"));
        return;
    }

    const PluginCapabilityId id = capability->identity();
    if (is_active(id))
        close(id);

    Request request;
    request.kind = RequestKind::Open;
    request.capability = capability;
    request.id = id;
    request.plate_index = plate_index;
    request.scene_id = scene_id;
    request.orca_version = std::move(orca_version);
    request.completion = std::move(completion);
    try {
        request.snapshot = std::make_shared<PreviewGeometrySnapshot::Snapshot>(
            PreviewGeometrySnapshot::capture(*mesh, result, plate_index, scene_id, printable_area));
        request.snapshot_path = prepare_cache(request.id.plugin_key, plate_index, scene_id);
        enqueue(std::move(request));
    } catch (const std::exception& error) {
        if (request.completion)
            request.completion(recoverable_error(request.id, "capture", error));
    }
}

void PluginVisualizations::request_updates(std::shared_ptr<const GUI::PreviewTriangleMesh> mesh,
                                           const GCodeProcessorResult& result, const Pointfs& printable_area, int plate_index, uint64_t scene_id,
                                           std::string orca_version, Completion completion)
{
    std::vector<std::shared_ptr<VisualizationPluginCapability>> capabilities_to_update;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& entry : m_sessions)
            if (entry.second.scene_id != scene_id)
                capabilities_to_update.push_back(entry.second.capability);
    }
    if (capabilities_to_update.empty())
        return;
    if (!mesh) {
        if (completion)
            completion(ExecutionResult::failure(PluginResult::RecoverableError, "Preview mesh is unavailable"));
        return;
    }

    std::shared_ptr<const PreviewGeometrySnapshot::Snapshot> snapshot;
    try {
        snapshot = std::make_shared<PreviewGeometrySnapshot::Snapshot>(
            PreviewGeometrySnapshot::capture(*mesh, result, plate_index, scene_id, printable_area));
    } catch (const std::exception& error) {
        if (completion)
            completion(recoverable_error(capabilities_to_update.front()->identity(), "capture", error));
        return;
    }

    for (auto& capability : capabilities_to_update) {
        Request request;
        request.kind = RequestKind::Update;
        request.capability = capability;
        request.id = capability->identity();
        request.snapshot = snapshot;
        request.plate_index = plate_index;
        request.scene_id = scene_id;
        request.orca_version = orca_version;
        request.snapshot_path = prepare_cache(request.id.plugin_key, plate_index, scene_id);
        request.completion = completion;
        enqueue(std::move(request));
    }
}

void PluginVisualizations::enqueue(Request request)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (request.kind != RequestKind::Open || m_sessions.count(request.id) == 0) {
            request.generation = ++m_generations[request.id];
            m_requests.erase(std::remove_if(m_requests.begin(), m_requests.end(), [&request](const Request& pending) {
                return pending.id == request.id;
            }), m_requests.end());
            m_requests.push_back(std::move(request));
            ensure_worker_locked();
            m_worker_cv.notify_one();
            return;
        }
    }
    if (request.completion)
        request.completion(ExecutionResult::skipped("Visualization session is already open"));
}

void PluginVisualizations::ensure_worker_locked()
{
    if (!m_worker.joinable())
        m_worker = std::thread([this] { worker_loop(); });
}

bool PluginVisualizations::is_current_locked(const Request& request) const
{
    const auto generation = m_generations.find(request.id);
    return generation != m_generations.end() && generation->second == request.generation;
}

void PluginVisualizations::worker_loop()
{
    for (;;) {
        Request request;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_worker_cv.wait(lock, [this] { return m_stop_worker || !m_requests.empty(); });
            if (m_stop_worker && m_requests.empty())
                return;
            request = std::move(m_requests.front());
            m_requests.pop_front();
        }

        try {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (!is_current_locked(request))
                    throw std::runtime_error("Visualization snapshot request was cancelled");
            }
            PreviewGeometrySnapshot::write_atomic(*request.snapshot, request.snapshot_path);
            request.snapshot.reset();
            dispatch_request(std::move(request));
        } catch (const std::exception& error) {
            ExecutionResult result = recoverable_error(request.id, "build", error);
            remove_snapshot(request.snapshot_path);
            finish_request(request, std::move(result));
        }
    }
}

void PluginVisualizations::dispatch_request(Request request)
{
    auto dispatch = [this, request = std::move(request)]() mutable {
        ExecutionResult result;
        try {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (!is_current_locked(request))
                    throw std::runtime_error("Visualization snapshot request was cancelled");
            }

            VisualizationContext context;
            context.orca_version = request.orca_version;
            context.scene_id = request.scene_id;
            context.plate_index = request.plate_index;
            context.geometry_path = request.snapshot_path.string();
            context.metadata_json = metadata_json(request.scene_id, request.plate_index);
            if (request.kind == RequestKind::Open)
                result = open(request.capability, context);
            else
                result = update(request.id, context);
        } catch (const std::exception& error) {
            result = recoverable_error(request.id, request.kind == RequestKind::Open ? "open" : "update", error);
        }

        if (result.status != PluginResult::Success)
            remove_snapshot(request.snapshot_path);
        finish_request(request, std::move(result));
    };

    if (wxTheApp != nullptr && !wxIsMainThread())
        wxTheApp->CallAfter(std::move(dispatch));
    else
        dispatch();
}

void PluginVisualizations::finish_request(const Request& request, ExecutionResult result)
{
    if (request.completion)
        request.completion(std::move(result));
}

boost::filesystem::path PluginVisualizations::prepare_cache(const std::string& plugin_key, int plate_index, uint64_t scene_id)
{
    namespace fs = boost::filesystem;
    const fs::path cache = fs::path(PluginManager::instance().get_storage_dir(plugin_key)) / "preview_cache";
    bool initialize = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        initialize = m_initialized_cache_plugins.insert(plugin_key).second;
    }
    if (initialize) {
        boost::system::error_code ignored;
        fs::remove_all(cache, ignored);
    }
    fs::create_directories(cache);
    return cache / ("plate-" + std::to_string(plate_index) + "-scene-" + std::to_string(scene_id) + ".orpm");
}

void PluginVisualizations::remove_snapshot(const boost::filesystem::path& path)
{
    if (path.empty())
        return;
    boost::system::error_code ignored;
    boost::filesystem::remove(path, ignored);
}

PluginVisualizations::ChangeToken PluginVisualizations::subscribe(std::function<void()> callback)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const ChangeToken token = m_next_token++;
    m_callbacks.emplace(token, std::move(callback));
    return token;
}

void PluginVisualizations::unsubscribe(ChangeToken token)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_callbacks.erase(token);
}

void PluginVisualizations::notify_change()
{
    std::vector<std::function<void()>> callbacks;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& entry : m_callbacks)
            callbacks.push_back(entry.second);
    }
    for (const auto& callback : callbacks)
        callback();
}

} // namespace Slic3r
