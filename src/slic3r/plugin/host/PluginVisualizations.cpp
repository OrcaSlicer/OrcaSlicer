#include "PluginVisualizations.hpp"

#include "PreviewGeometrySnapshot.hpp"
#include "../../GUI/GUI_App.hpp"
#include "../../GUI/LibVGCode/LibVGCodeWrapper.hpp"
#include "../../GUI/PartPlate.hpp"
#include "../../GUI/Plater.hpp"
#include "../../GUI/ToolpathMeshBuilder.hpp"
#include "../PluginManager.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/libslic3r.h"

#include <libvgcode/include/Viewer.hpp>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
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

bool negotiate_input(const VisualizationPluginCapability& capability, const VisualizationResourceRequest& request,
                     VisualizationInput& input)
{
    for (const VisualizationInputSpec& spec : capability.supported_inputs()) {
        if (spec.kind != request.kind || spec.transport != VisualizationInputs::FILE_TRANSPORT)
            continue;
        const PreviewGeometrySnapshot::Format* format = PreviewGeometrySnapshot::find_format(spec.format);
        if (format == nullptr)
            continue;
        VisualizationInput candidate{request.kind, format->media_type, VisualizationInputs::FILE_TRANSPORT, {},
                                     format->major_version, format->minor_version, request.scope, {}};
        if (capability.supports(candidate)) {
            input = std::move(candidate);
            return true;
        }
    }
    return false;
}

TriangleMesh capture_model_plate(GUI::PartPlate& plate, const Model& model)
{
    std::vector<std::pair<size_t, size_t>> instances;
    for (size_t object_index = 0; object_index < model.objects.size(); ++object_index) {
        const ModelObject* object = model.objects[object_index];
        for (size_t instance_index = 0; instance_index < object->instances.size(); ++instance_index) {
            const ModelInstance* instance = object->instances[instance_index];
            if (plate.contain_instance(int(object_index), int(instance_index)) && instance->is_printable())
                instances.emplace_back(object_index, instance_index);
        }
    }
    return PreviewGeometrySnapshot::capture_model(model, instances);
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
        }
    });
    manager.subscribe_on_capability_unload_callback([this](const PluginCapabilityId& id) {
        if (id.type == PluginCapabilityType::Visualization)
            close(id);
    });
    manager.subscribe_on_unload_callback([this](const std::string& plugin_key) {
        close_plugin(plugin_key);
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

std::vector<std::shared_ptr<VisualizationPluginCapability>> PluginVisualizations::capabilities_for(const VisualizationInput& input) const
{
    auto result = capabilities();
    result.erase(std::remove_if(result.begin(), result.end(), [&input](const auto& capability) {
        return !capability->supports(input);
    }), result.end());
    return result;
}

ExecutionResult PluginVisualizations::open(const std::shared_ptr<VisualizationPluginCapability>& capability, const VisualizationContext& ctx,
                                           boost::filesystem::path owned_resource)
{
    std::vector<boost::filesystem::path> paths;
    if (!owned_resource.empty())
        paths.push_back(std::move(owned_resource));
    return open_resources(capability, ctx, std::move(paths), capability ? capability->requested_resources() : std::vector<VisualizationResourceRequest>{});
}

ExecutionResult PluginVisualizations::open_resources(const std::shared_ptr<VisualizationPluginCapability>& capability,
                                                     const VisualizationContext& supplied,
                                                     std::vector<boost::filesystem::path> owned_resources,
                                                     std::vector<VisualizationResourceRequest> resource_requests)
{
    if (!capability || !capability->is_enabled())
        return ExecutionResult::skipped("Visualization capability is unavailable");
    VisualizationContext ctx = supplied;
    if (ctx.resources.empty() && !ctx.input.kind.empty())
        ctx.resources.push_back(ctx.input);
    if (ctx.resources.empty())
        return ExecutionResult::skipped("Visualization has no resources");
    for (const VisualizationInput& input : ctx.resources)
        if (!capability->supports(input))
            return ExecutionResult::skipped("Visualization capability does not support this input");
    ctx.input = ctx.resources.front();

    const PluginCapabilityId id = capability->identity();
    ExecutionResult result;
    try {
        std::lock_guard<std::recursive_mutex> dispatch_lock(m_dispatch_mutex);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_sessions.count(id) != 0)
                return ExecutionResult::skipped("Visualization session is already open");
        }
        PluginCapabilityInterface::RefCounter ref_counter(*capability);
        result = capability->open(ctx);
        if (result.status == PluginResult::Success) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_sessions.emplace(id, Session{capability, ctx.revision, std::move(owned_resources), ctx.resources,
                                           std::move(resource_requests)});
        }
    } catch (const std::exception& error) {
        return recoverable_error(id, "open", error);
    }
    return result;
}

ExecutionResult PluginVisualizations::update(const PluginCapabilityId& id, const VisualizationContext& ctx,
                                             boost::filesystem::path owned_resource)
{
    std::vector<boost::filesystem::path> paths;
    if (!owned_resource.empty())
        paths.push_back(std::move(owned_resource));
    return update_resources(id, ctx, std::move(paths), {});
}

ExecutionResult PluginVisualizations::update_resources(const PluginCapabilityId& id, const VisualizationContext& supplied,
                                                       std::vector<boost::filesystem::path> owned_resources,
                                                       std::vector<VisualizationResourceRequest> resource_requests)
{
    std::shared_ptr<VisualizationPluginCapability> capability;
    ExecutionResult result;
    std::vector<boost::filesystem::path> old_snapshots;
    VisualizationContext ctx = supplied;
    if (ctx.resources.empty() && !ctx.input.kind.empty())
        ctx.resources.push_back(ctx.input);
    if (ctx.resources.empty())
        return ExecutionResult::skipped("Visualization has no resources");
    ctx.input = ctx.resources.front();
    try {
        std::lock_guard<std::recursive_mutex> dispatch_lock(m_dispatch_mutex);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const auto it = m_sessions.find(id);
            if (it == m_sessions.end())
                return ExecutionResult::skipped("Visualization session is not open");
            if (it->second.revision == ctx.revision)
                return ExecutionResult::skipped("Visualization scene is unchanged");
            capability = it->second.capability;
            if (resource_requests.empty())
                resource_requests = it->second.resource_requests;
        }
        for (const VisualizationInput& input : ctx.resources)
            if (!capability->supports(input))
                return ExecutionResult::skipped("Visualization capability does not support this input");

        PluginCapabilityInterface::RefCounter ref_counter(*capability);
        result = capability->update(ctx);
        if (result.status == PluginResult::FatalError) {
            close(id);
            return result;
        }
        if (result.status == PluginResult::Success) {
            std::lock_guard<std::mutex> lock(m_mutex);
            const auto it = m_sessions.find(id);
            if (it != m_sessions.end() && it->second.capability == capability) {
                old_snapshots = std::move(it->second.snapshot_paths);
                it->second.revision = ctx.revision;
                it->second.snapshot_paths = std::move(owned_resources);
                it->second.inputs = ctx.resources;
                it->second.resource_requests = std::move(resource_requests);
            }
        }
    } catch (const std::exception& error) {
        return recoverable_error(id, "update", error);
    }
    remove_snapshots(old_snapshots);
    return result;
}

void PluginVisualizations::close(const PluginCapabilityId& id)
{
    std::lock_guard<std::recursive_mutex> dispatch_lock(m_dispatch_mutex);
    std::shared_ptr<VisualizationPluginCapability> capability;
    std::vector<boost::filesystem::path> snapshot_paths;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_generations[id];
        m_requests.erase(std::remove_if(m_requests.begin(), m_requests.end(), [&id](const Request& request) { return request.id == id; }),
                         m_requests.end());
        const auto it = m_sessions.find(id);
        if (it == m_sessions.end())
            return;
        capability = std::move(it->second.capability);
        snapshot_paths = std::move(it->second.snapshot_paths);
        m_sessions.erase(it);
    }

    try {
        PluginCapabilityInterface::RefCounter ref_counter(*capability);
        capability->close();
    } catch (const std::exception& error) {
        recoverable_error(id, "close", error);
    }
    remove_snapshots(snapshot_paths);
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
                                        const std::vector<VisualizationResourceRequest>& resources, Completion completion)
{
    if (!capability || !capability->is_enabled()) {
        if (completion)
            completion(ExecutionResult::skipped("Visualization capability is unavailable"));
        return;
    }
    const auto requested = resources.empty() ? capability->requested_resources() : resources;
    auto capture = [this, capability, requested, completion = std::move(completion)]() mutable {
        capture_and_enqueue(capability, requested, RequestKind::Open, std::move(completion));
    };
    if (wxTheApp != nullptr && !wxIsMainThread())
        wxTheApp->CallAfter(std::move(capture));
    else
        capture();
}

bool PluginVisualizations::request_update(const PluginCapabilityId& id,
                                          const std::vector<VisualizationResourceRequest>& resources)
{
    std::shared_ptr<VisualizationPluginCapability> capability;
    std::vector<VisualizationResourceRequest> requested = resources;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_sessions.find(id);
        if (it == m_sessions.end())
            return false;
        capability = it->second.capability;
        if (requested.empty())
            requested = it->second.resource_requests;
    }
    auto capture = [this, capability = std::move(capability), requested = std::move(requested)]() mutable {
        capture_and_enqueue(capability, requested, RequestKind::Update);
    };
    if (wxTheApp != nullptr && !wxIsMainThread())
        wxTheApp->CallAfter(std::move(capture));
    else
        capture();
    return true;
}

void PluginVisualizations::capture_and_enqueue(const std::shared_ptr<VisualizationPluginCapability>& capability,
                                               const std::vector<VisualizationResourceRequest>& resource_requests,
                                               RequestKind kind, Completion completion)
{
    if (!wxTheApp || !GUI::wxGetApp().plater()) {
        if (completion)
            completion(ExecutionResult::skipped("Visualization resources require the editor"));
        return;
    }
    if (resource_requests.empty()) {
        if (completion)
            completion(ExecutionResult::skipped("Visualization requested no resources"));
        return;
    }

    GUI::Plater* plater = GUI::wxGetApp().plater();
    GUI::PartPlateList& plate_list = plater->get_partplate_list();
    Request request;
    request.kind = kind;
    request.capability = capability;
    request.id = capability->identity();
    request.orca_version = SoftFever_VERSION;
    request.resource_requests = resource_requests;
    request.completion = std::move(completion);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        request.revision = m_next_revision++;
    }

    try {
        for (const VisualizationResourceRequest& requested : resource_requests) {
            std::vector<std::pair<int, GUI::PartPlate*>> plates;
            if (requested.scope == VisualizationInputs::CURRENT_PLATE) {
                plates.emplace_back(plate_list.get_curr_plate_index(), plate_list.get_curr_plate());
            } else if (requested.scope == VisualizationInputs::PROJECT) {
                const auto& all_plates = plate_list.get_plate_list();
                for (size_t index = 0; index < all_plates.size(); ++index)
                    plates.emplace_back(int(index), all_plates[index]);
            } else {
                throw std::runtime_error("Unsupported visualization resource scope: " + requested.scope);
            }

            for (const auto& indexed_plate : plates) {
                const int plate_index = indexed_plate.first;
                GUI::PartPlate* plate = indexed_plate.second;
                PendingResource resource;
                resource.plate_index = plate_index;
                if (!negotiate_input(*capability, requested, resource.input))
                    throw std::runtime_error("Visualization capability supports no host format for " + requested.kind);
                resource.input.metadata["plate_index"] = std::to_string(plate_index);

                if (requested.kind == VisualizationInputs::MODEL) {
                    TriangleMesh model_mesh = capture_model_plate(*plate, plater->model());
                    if (model_mesh.empty())
                        continue;
                    resource.model = std::make_shared<const TriangleMesh>(std::move(model_mesh));
                } else if (requested.kind == VisualizationInputs::TOOLPATH) {
                    GCodeProcessorResult* result = plate->get_slice_result();
                    if (!plate->is_slice_result_valid() || result == nullptr || result->moves.empty())
                        continue;
                    const std::vector<std::string> colors = plater->get_extruder_colors_from_plater_config(result);
                    libvgcode::Viewer viewer;
                    libvgcode::GCodeInputData data = libvgcode::convert(*result, colors, {}, viewer);
                    auto mesh = GUI::build_preview_triangle_mesh(data);
                    resource.toolpath = std::make_shared<PreviewGeometrySnapshot::Snapshot>(
                        PreviewGeometrySnapshot::capture(mesh, *result, plate_index, result->id, plate->get_shape()));
                } else {
                    throw std::runtime_error("Unsupported visualization resource kind: " + requested.kind);
                }

                const auto* format = PreviewGeometrySnapshot::find_format(resource.input.format);
                resource.snapshot_path = prepare_cache(request.id.plugin_key, plate_index, request.revision, format->extension);
                resource.input.location = resource.snapshot_path.string();
                request.resources.push_back(std::move(resource));
            }
        }
        if (request.resources.empty()) {
            if (request.completion)
                request.completion(ExecutionResult::skipped("No requested visualization resources are available"));
            return;
        }
        if (kind == RequestKind::Open && is_active(request.id))
            close(request.id);
        enqueue(std::move(request));
    } catch (const std::exception& error) {
        if (request.completion)
            request.completion(recoverable_error(request.id, "capture", error));
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
            for (PendingResource& resource : request.resources) {
                if (resource.toolpath)
                    PreviewGeometrySnapshot::write_atomic(*resource.toolpath, resource.snapshot_path, resource.input.format);
                else if (resource.model)
                    PreviewGeometrySnapshot::write_atomic(*resource.model, resource.snapshot_path, resource.input.format);
                else
                    throw std::runtime_error("Visualization resource has no geometry");
                resource.toolpath.reset();
                resource.model.reset();
            }
            dispatch_request(std::move(request));
        } catch (const std::exception& error) {
            ExecutionResult result = recoverable_error(request.id, "build", error);
            for (const PendingResource& resource : request.resources)
                remove_snapshot(resource.snapshot_path);
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
            context.revision = request.revision;
            std::vector<boost::filesystem::path> paths;
            for (const PendingResource& resource : request.resources) {
                context.resources.push_back(resource.input);
                paths.push_back(resource.snapshot_path);
            }
            context.input = context.resources.front();
            const auto plate = context.input.metadata.find("plate_index");
            if (plate != context.input.metadata.end())
                context.metadata.emplace("plate_index", plate->second);
            if (request.kind == RequestKind::Open)
                result = open_resources(request.capability, context, std::move(paths), request.resource_requests);
            else
                result = update_resources(request.id, context, std::move(paths), request.resource_requests);
        } catch (const std::exception& error) {
            result = recoverable_error(request.id, request.kind == RequestKind::Open ? "open" : "update", error);
        }

        if (result.status != PluginResult::Success)
            for (const PendingResource& resource : request.resources)
                remove_snapshot(resource.snapshot_path);
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

boost::filesystem::path PluginVisualizations::prepare_cache(const std::string& plugin_key, int plate_index, uint64_t revision,
                                                            const std::string& extension)
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
    return cache / ("plate-" + std::to_string(plate_index) + "-revision-" + std::to_string(revision) +
                    fs::unique_path("-%%%%-%%%%-%%%%").string() + extension);
}

void PluginVisualizations::remove_snapshot(const boost::filesystem::path& path)
{
    if (path.empty())
        return;
    boost::system::error_code ignored;
    boost::filesystem::remove(path, ignored);
}

void PluginVisualizations::remove_snapshots(const std::vector<boost::filesystem::path>& paths)
{
    for (const boost::filesystem::path& path : paths)
        remove_snapshot(path);
}

} // namespace Slic3r
