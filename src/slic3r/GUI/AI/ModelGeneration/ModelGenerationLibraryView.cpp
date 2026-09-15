#include "slic3r/GUI/ModelGenerationPanel.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/AI/AIWindowAppearance.hpp"
#include "ModelLibraryThumbnail.hpp"
#include "ModelGenerationPresentation.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <optional>
#include <wx/button.h>
#include <wx/choice.h>
#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/datetime.h>
#include <wx/dcbuffer.h>
#include <wx/image.h>
#include <wx/notebook.h>
#include <wx/log.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/weakref.h>

namespace Slic3r::GUI {
using namespace ModelGenerationPresentation;

std::vector<ModelGenerationPanel::GeneratedModelEntry> ModelGenerationPanel::read_library_entries(
    const boost::filesystem::path& root, const std::atomic<bool>& cancelled)
{
    // This snapshot reader runs on the history worker. Resolve the output root
    // before dispatch and do not read app configuration or create directories.
    const boost::filesystem::path downloads = root / "downloads";
    const auto temp_path = [&downloads](const std::string& id, const std::string& extension) {
        return downloads / ("orcaslicer-ai-" + id + "." + extension);
    };
    const auto library_metadata_path = [&temp_path](const std::string& id) { return temp_path(id, "json"); };
    const auto is_supported_image = is_library_image_file;
    const auto library_image_path = [&is_supported_image](const nlohmann::json& data, const char* key,
                                                        const boost::filesystem::path& base) {
        const auto value = data.find(key);
        if (value == data.end() || !value->is_string() || value->get_ref<const std::string&>().empty())
            return boost::filesystem::path();
        const auto path = base / value->get<std::string>();
        return path_is_inside(base, path) && is_supported_image(path) ? path : boost::filesystem::path();
    };
    const auto read_json = [](const boost::filesystem::path& path) {
        boost::system::error_code error;
        const auto bytes = boost::filesystem::file_size(path, error);
        if (error || bytes > 1024 * 1024)
            return nlohmann::json();
        nlohmann::json value = ModelGenerationPresentation::read_json(path);
        if (!value.is_object())
            return value;

        // A single hand-edited or partially-written metadata file must not
        // abort the complete history scan.  The model path remains useful even
        // when optional metadata has the wrong JSON type; remove only fields
        // whose typed access below would otherwise throw.
        const auto erase_unless = [&value](const char* key, const auto& predicate) {
            const auto it = value.find(key);
            if (it != value.end() && !predicate(*it))
                value.erase(it);
        };
        erase_unless("generated_at", [](const nlohmann::json& item) {
            return item.is_number_integer() || item.is_number_unsigned();
        });
        erase_unless("imported_at", [](const nlohmann::json& item) {
            return item.is_number_integer() || item.is_number_unsigned();
        });
        erase_unless("triangle_count", [](const nlohmann::json& item) {
            return item.is_number_integer() || item.is_number_unsigned();
        });
        erase_unless("load_seconds", [](const nlohmann::json& item) { return item.is_number(); });
        for (const char* key : {"print_feedback", "provider", "provider_task_id",
                                "provider_conversion_task_id", "prompt", "reference_image_path",
                                "ai_image_path", "color_intent_path", "color_intent_schema",
                                "color_intent_sha256", "job_id", "model_path", "source"}) {
            erase_unless(key, [](const nlohmann::json& item) { return item.is_string(); });
        }
        erase_unless("use_printable_colors", [](const nlohmann::json& item) { return item.is_boolean(); });
        erase_unless("palette_constrained", [](const nlohmann::json& item) { return item.is_boolean(); });
        return value;
    };
    boost::system::error_code ec;
    if (cancelled || !boost::filesystem::is_directory(root, ec)) return {};

    std::map<std::string, boost::filesystem::path> models;
    for (boost::filesystem::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (cancelled) return {};
        boost::system::error_code entry_ec;
        if (!boost::filesystem::is_directory(it->path(), entry_ec))
            continue;
        const std::string job_id = it->path().filename().string();
        if (job_id == "downloads" || job_id.rfind("attempt-", 0) == 0)
            continue;
        boost::filesystem::path model_path = it->path() / "model.glb";
        if (!is_nonempty_model(model_path)) model_path = it->path() / "model-vertex-color.obj";
        if (boost::filesystem::is_regular_file(model_path, entry_ec) &&
            boost::filesystem::file_size(model_path, entry_ec) > 0 && !entry_ec)
            models.emplace(job_id, model_path);
    }
    ec.clear();
    if (boost::filesystem::is_directory(downloads, ec)) {
        for (boost::filesystem::directory_iterator it(downloads, ec), end; !ec && it != end; it.increment(ec)) {
            if (cancelled) return {};
            const boost::filesystem::path path = it->path();
            boost::system::error_code entry_ec;
            if (!boost::filesystem::is_regular_file(path, entry_ec) || !AI::is_model_artifact(path))
                continue;
            const std::string job_id = download_job_id(path);
            if (job_id.rfind("finish-", 0) == 0 && !read_json(library_metadata_path(job_id)).is_object())
                continue; // Unaccepted finishing previews are never history entries.
            if (!job_id.empty() && boost::filesystem::file_size(path, entry_ec) > 0 && !entry_ec)
                models.emplace(job_id, path);
        }
    }

    // A finishing OBJ stays beside its source so relative materials remain
    // valid. Accepted metadata, not its temporary filename, registers it.
    const auto records = library_metadata_path("placeholder").parent_path();
    ec.clear();
    if (boost::filesystem::is_directory(records, ec)) {
        for (boost::filesystem::directory_iterator it(records, ec), end; !ec && it != end; it.increment(ec)) {
            if (cancelled) return {};
            if (it->path().extension() != ".json") continue;
            const auto data = read_json(it->path());
            if (!data.is_object() || data.value("source", std::string()) != "local_finishing") continue;
            const auto id = data.value("job_id", std::string());
            const auto path = root / data.value("model_path", std::string());
            if (id.rfind("finish-", 0) == 0 && path_is_inside(root, path) && is_nonempty_model(path))
                models[id] = path;
        }
    }
    std::vector<GeneratedModelEntry> entries;
    entries.reserve(models.size());
    ec.clear();
    for (boost::filesystem::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (cancelled) return {};
        const auto job_id = it->path().filename().string();
        if (models.count(job_id)) continue;
        const auto design = read_design_history_entry(root, job_id, false);
        if (!design) continue;
        GeneratedModelEntry entry;
        entry.design_only = true;
        entry.job_id = job_id;
        entry.generated_at = design->generated_at;
        entry.reference_image_path = design->input_path;
        entry.preview_path = design->preview_path;
        entry.ai_image_path = design->raw_preview_path;
        entry.title = wxString::FromUTF8(design->prompt);
        if (entry.title.empty()) entry.title = _L("AI 设计图 ") + wxString::FromUTF8(job_id.substr(0, 8));
        if (entry.title.length() > 32) entry.title = entry.title.Left(32) + _L("…");
        const wxDateTime generated(entry.generated_at);
        entry.details = generated.IsValid() ? generated.FormatISODate() + " " + generated.FormatISOTime() : _L("未知时间");
        entry.details += design->state == "awaiting_confirmation" ? _L(" · 设计待确认")
            : design->state == "stopped" ? _L(" · 已停止，设计图已保留") : _L(" · 未完成，设计图已保留");
        entry.details += _L("\n素材：");
        entry.details += entry.reference_image_path.empty() ? _L("无参考图") : _L("原图 ✓");
        entry.details += _L(" · AI 图 ✓ · 尚无 3D 模型");
        entries.emplace_back(std::move(entry));
    }
    for (const auto& [job_id, model_path] : models) {
        if (cancelled) return {};
        boost::system::error_code entry_ec;
        GeneratedModelEntry entry;
        entry.job_id = job_id;
        entry.model_path = model_path;
        entry.generated_at = boost::filesystem::last_write_time(model_path, entry_ec);
        if (entry_ec)
            entry.generated_at = 0;

        const nlohmann::json metadata = read_json(library_metadata_path(job_id));
        if (metadata.is_object()) {
            entry.generated_at = metadata.value("generated_at", entry.generated_at);
            entry.imported_at = metadata.value("imported_at", std::time_t {0});
            entry.triangle_count = metadata.value("triangle_count", size_t {0});
            entry.load_seconds = metadata.value("load_seconds", 0.0);
            entry.print_feedback = metadata.value("print_feedback", std::string());
            entry.use_printable_colors = metadata.value("use_printable_colors", false);
            const std::string provider = metadata.value("provider", std::string());
            const std::string provider_task_id = metadata.value("provider_task_id", std::string());
            const std::string provider_conversion_task_id =
                metadata.value("provider_conversion_task_id", std::string());
            if ((provider == "tripo" || provider == "hunyuan") && valid_provider_task_id(provider_task_id)) {
                entry.provider_name = provider;
                entry.provider_task_id = provider_task_id;
                if (valid_provider_task_id(provider_conversion_task_id))
                    entry.provider_conversion_task_id = provider_conversion_task_id;
            }
            std::string prompt = metadata.value("prompt", std::string());
            if (prompt == INTERNAL_DEFAULT_IMAGE_INSTRUCTION)
                prompt.clear();
            if (!prompt.empty()) {
                entry.title = wxString::FromUTF8(prompt);
                if (entry.title.length() > 32)
                    entry.title = entry.title.Left(32) + _L("…");
            }
            const auto palette = metadata.find("palette");
            if (palette != metadata.end() && palette->is_array()) {
                for (const auto& color : *palette) {
                    if (color.is_string())
                        entry.palette.push_back(color.get<std::string>());
                }
            }
            const auto roles = metadata.find("palette_roles");
            if (roles != metadata.end() && roles->is_object()) {
                for (const char* role : PALETTE_ROLE_IDS) {
                    const auto color = roles->find(role);
                    if (color != roles->end() && color->is_string())
                        entry.palette_roles.emplace(role, color->get<std::string>());
                }
            }
            entry.reference_image_path = library_image_path(metadata, "reference_image_path", root);
            entry.ai_image_path = library_image_path(metadata, "ai_image_path", root);
            const auto color_intent_path = metadata.find("color_intent_path");
            if (color_intent_path != metadata.end() && color_intent_path->is_string()) {
                const boost::filesystem::path candidate = root / color_intent_path->get<std::string>();
                if (path_is_inside(root, candidate) && boost::filesystem::is_regular_file(candidate, entry_ec))
                    entry.color_intent_path = candidate;
            }
            const auto color_intent_schema = metadata.find("color_intent_schema");
            const auto color_intent_sha256 = metadata.find("color_intent_sha256");
            if (color_intent_schema != metadata.end() && color_intent_schema->is_string())
                entry.color_intent_schema = color_intent_schema->get<std::string>();
            if (color_intent_sha256 != metadata.end() && color_intent_sha256->is_string())
                entry.color_intent_sha256 = color_intent_sha256->get<std::string>();
        }

        const boost::filesystem::path job_preview = root / job_id / "preview.png";
        const boost::filesystem::path download_preview = temp_path(job_id, "png");
        entry_ec.clear();
        if (boost::filesystem::is_regular_file(job_preview, entry_ec))
            entry.preview_path = job_preview;
        else {
            entry_ec.clear();
            if (boost::filesystem::is_regular_file(download_preview, entry_ec))
                entry.preview_path = download_preview;
        }
        if (entry.reference_image_path.empty()) {
            const boost::filesystem::path legacy_input = temp_path(job_id + "-input", "png");
            if (is_supported_image(legacy_input) && path_is_inside(root, legacy_input))
                entry.reference_image_path = legacy_input;
        }
        if (entry.ai_image_path.empty()) {
            const boost::filesystem::path legacy_raw = temp_path(job_id + "-raw", "png");
            if (is_supported_image(legacy_raw) && path_is_inside(root, legacy_raw))
                entry.ai_image_path = legacy_raw;
            else if (is_supported_image(entry.preview_path) && path_is_inside(root, entry.preview_path))
                entry.ai_image_path = entry.preview_path;
        }

        if (entry.palette.empty()) {
            const nlohmann::json preview_colors = read_json(root / job_id / "preview-colors.json");
            if (preview_colors.is_object() && preview_colors.value("palette_constrained", true)) {
                const auto pixels = preview_colors.find("palette_pixels");
                if (pixels != preview_colors.end() && pixels->is_object()) {
                    for (auto color = pixels->begin(); color != pixels->end(); ++color)
                        entry.palette.push_back(color.key());
                }
            }
            entry.use_printable_colors = !entry.palette.empty();
        }
        for (auto role = entry.palette_roles.begin(); role != entry.palette_roles.end();) {
            const bool matches_palette = std::any_of(
                entry.palette.begin(), entry.palette.end(), [&role](const std::string& color) {
                    return same_palette_color(color, role->second);
                });
            if (!matches_palette)
                role = entry.palette_roles.erase(role);
            else
                ++role;
        }
        if (entry.palette_roles.empty())
            entry.palette_roles = automatic_palette_roles(entry.palette);

        if (entry.title.empty())
            entry.title = _L("AI 模型 ") + wxString::FromUTF8(job_id.substr(0, std::min<size_t>(8, job_id.size())));
        wxDateTime generated(entry.generated_at);
        const wxString date = generated.IsValid()
            ? generated.FormatISODate() + " " + generated.FormatISOTime()
            : _L("未知时间");
        entry_ec.clear();
        const auto model_size = boost::filesystem::file_size(model_path, entry_ec);
        const double megabytes = entry_ec ? 0.0 : double(model_size) / (1024.0 * 1024.0);
        entry.details = date + wxString::Format(_L(" · %.1f MB · "), megabytes) +
            (entry.use_printable_colors
                ? wxString::Format(_L("%llu 种可打印颜色"), static_cast<unsigned long long>(entry.palette.size()))
                : _L("自然颜色"));
        if (entry.triangle_count > 0)
            entry.details += wxString::Format(
                _L(" · %.1f 万面"), static_cast<double>(entry.triangle_count) / 10000.0);
        if (entry.load_seconds > 0.0)
            entry.details += wxString::Format(_L(" · 上次加载 %.2f 秒"), entry.load_seconds);
        entry.details += _L("\n素材：");
        entry.details += entry.reference_image_path.empty() ? _L("原图未保存") : _L("原图 ✓");
        entry.details += _L(" · ");
        entry.details += entry.ai_image_path.empty() ? _L("AI 图未保存") : _L("AI 图 ✓");
        if (!entry.color_intent_path.empty() || !entry.color_intent_schema.empty() ||
            !entry.color_intent_sha256.empty())
            entry.details += AI::is_valid_color_intent_manifest_ref(
                {entry.color_intent_path.string(), entry.color_intent_schema, entry.color_intent_sha256})
                ? _L(" · 颜色意图 ✓") : _L(" · 颜色意图无效");
        if (entry.imported_at > 0) {
            entry.details += _L("\n已导入准备页");
            if (entry.print_feedback == "success")
                entry.details += _L(" · 打印反馈：成功");
            else if (entry.print_feedback == "issue")
                entry.details += _L(" · 打印反馈：有问题");
            else
                entry.details += _L(" · 尚未记录打印结果");
        }

        entries.emplace_back(std::move(entry));
    }

    std::sort(entries.begin(), entries.end(), [](const GeneratedModelEntry& lhs, const GeneratedModelEntry& rhs) {
        if (lhs.generated_at != rhs.generated_at)
            return lhs.generated_at > rhs.generated_at;
        return lhs.job_id < rhs.job_id;
    });
    return entries;
}

namespace {

// Keep thumbnail painting in one fixed, clipped surface, independently of the
// native title/button controls and the adjacent notebook's OpenGL preview.
class LibraryThumbnail final : public wxPanel
{
public:
    explicit LibraryThumbnail(wxWindow* parent) : wxPanel(parent)
    {
        SetMinSize(FromDIP(wxSize(96, 96)));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            dc.SetBackground(wxBrush(GetBackgroundColour()));
            dc.Clear();
            const auto size = GetClientSize();
            if (m_bitmap.IsOk()) {
                dc.DrawBitmap(m_bitmap, (size.x - m_bitmap.GetLogicalWidth()) / 2,
                              (size.y - m_bitmap.GetLogicalHeight()) / 2, true);
            } else {
                dc.SetFont(GetFont());
                dc.SetTextForeground(GetForegroundColour());
                dc.DrawLabel(m_pending ? _L("加载缩略图…") : _L("无缩略图"), wxRect(size), wxALIGN_CENTER);
            }
        });
    }

    void set_image(const wxImage& image)
    {
        m_pending = false;
        m_bitmap = image.IsOk() ? wxBitmap(image) : wxBitmap();
        if (m_bitmap.IsOk()) m_bitmap.SetScaleFactor(GetContentScaleFactor());
        else SetToolTip(_L("缩略图不可用（文件缺失、损坏或过大），可尝试双击打开资产。"));
        Refresh(false);
    }

    void finish_pending()
    {
        if (m_pending) set_image(wxImage());
    }

private:
    wxBitmap m_bitmap;
    bool m_pending {true};
};

} // namespace

struct ModelGenerationPanel::LibraryLoadState
{
    struct Request {
        uint64_t revision {0};
        bool scan {false};
        boost::filesystem::path root;
        std::vector<GeneratedModelEntry> entries;
        int edge {96};
        std::atomic<bool> cancelled {false};
    };
    struct Snapshot {
        uint64_t revision;
        std::vector<GeneratedModelEntry> entries;
        bool failed {false};
    };
    // Only plain pixels cross the worker/UI boundary. wxImage reference data
    // is not atomic in every supported wx build.
    struct Thumbnail {
        uint64_t revision;
        size_t index;
        int width {0}, height {0};
        std::vector<unsigned char> rgb, alpha;
    };
    std::mutex mutex;
    std::condition_variable wake;
    bool stopping {false};
    std::shared_ptr<Request> pending, active;
    std::optional<Snapshot> snapshot;
    std::vector<Thumbnail> thumbnails;
};

void ModelGenerationPanel::start_library_worker()
{
    if (m_library_load_state) return;
    m_library_load_state = std::make_shared<LibraryLoadState>();
    m_library_worker = std::thread([state = m_library_load_state] {
        ModelLibraryThumbnailCache cache;
        for (;;) {
            std::shared_ptr<LibraryLoadState::Request> request;
            {
                std::unique_lock<std::mutex> lock(state->mutex);
                state->wake.wait(lock, [&] { return state->stopping || state->pending; });
                if (state->stopping) return;
                request = std::move(state->pending);
                state->active = request;
            }
            if (request->scan) {
                LibraryLoadState::Snapshot snapshot {request->revision, {}};
                try { snapshot.entries = read_library_entries(request->root, request->cancelled); }
                catch (...) { snapshot.failed = true; }
                std::lock_guard<std::mutex> lock(state->mutex);
                if (!state->stopping && !request->cancelled) state->snapshot = std::move(snapshot);
            } else {
                for (size_t index = 0; index < request->entries.size() && !request->cancelled; ++index) {
                    const auto& entry = request->entries[index];
                    wxImage image;
                    try { image = cache.load(entry.ai_image_path, entry.reference_image_path, request->edge, request->cancelled); }
                    catch (...) { /* An unavailable thumbnail never removes an asset. */ }
                    LibraryLoadState::Thumbnail thumbnail {request->revision, index};
                    if (image.IsOk() && !request->cancelled) {
                        thumbnail.width = image.GetWidth();
                        thumbnail.height = image.GetHeight();
                        const size_t pixels = size_t(thumbnail.width) * thumbnail.height;
                        thumbnail.rgb.assign(image.GetData(), image.GetData() + pixels * 3);
                        if (image.HasAlpha()) thumbnail.alpha.assign(image.GetAlpha(), image.GetAlpha() + pixels);
                    }
                    std::lock_guard<std::mutex> lock(state->mutex);
                    if (state->stopping || request->cancelled) break;
                    state->thumbnails.push_back(std::move(thumbnail));
                }
            }
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (state->active == request) state->active.reset();
            }
        }
    });
}

void ModelGenerationPanel::cancel_library_loading()
{
    ++m_library_revision;
    m_library_refresh_pending = true;
    m_library_timer.Stop();
    if (!m_library_load_state) return;
    std::lock_guard<std::mutex> lock(m_library_load_state->mutex);
    if (m_library_load_state->active) m_library_load_state->active->cancelled = true;
    if (m_library_load_state->pending) m_library_load_state->pending->cancelled = true;
    m_library_load_state->pending.reset();
    m_library_load_state->snapshot.reset();
    m_library_load_state->thumbnails.clear();
}

void ModelGenerationPanel::stop_library_loading()
{
    cancel_library_loading();
    if (!m_library_load_state) return;
    {
        std::lock_guard<std::mutex> lock(m_library_load_state->mutex);
        m_library_load_state->stopping = true;
    }
    m_library_load_state->wake.notify_one();
    // Cancellation is checked between files. Each decode is bounded by file
    // bytes and pixel dimensions, and no worker callback owns a wxWindow.
    if (m_library_worker.joinable()) m_library_worker.join();
    m_library_load_state.reset();
}

void ModelGenerationPanel::load_library_entries()
{
    if (m_shutdown) return;
    if (!m_page_initialized || !m_library_scroller || !m_library_scroller->IsShownOnScreen()) {
        cancel_library_loading();
        return;
    }
    cancel_library_loading();
    start_library_worker();
    auto request = std::make_shared<LibraryLoadState::Request>();
    request->revision = m_library_revision;
    request->scan = true;
    request->root = ModelGenerationPresentation::generated_models_root();
    {
        std::lock_guard<std::mutex> lock(m_library_load_state->mutex);
        m_library_load_state->pending = std::move(request);
    }
    m_library_empty->SetLabel(_L("正在读取历史资产…"));
    m_library_empty->Show();
    m_library_previous->Disable();
    m_library_next->Disable();
    m_library_scroller->GetParent()->Layout();
    m_library_load_state->wake.notify_one();
    m_library_timer.Start(50);
}

void ModelGenerationPanel::on_library_timer(wxTimerEvent&)
{
    if (m_shutdown || !m_library_load_state) return;
    if (!m_library_scroller->IsShownOnScreen()) {
        cancel_library_loading();
        return;
    }
    std::optional<LibraryLoadState::Snapshot> snapshot;
    std::vector<LibraryLoadState::Thumbnail> thumbnails;
    {
        std::lock_guard<std::mutex> lock(m_library_load_state->mutex);
        snapshot = std::move(m_library_load_state->snapshot);
        m_library_load_state->snapshot.reset();
        thumbnails.swap(m_library_load_state->thumbnails);
    }
    if (snapshot && snapshot->revision == m_library_revision) {
        if (snapshot->failed) {
            m_library_empty->SetLabel(_L("历史资产读取失败，已保留当前列表。请点击刷新重试。"));
            m_library_empty->Show();
            m_library_previous->Enable(m_library_page > 0);
            m_library_next->Enable((m_library_page + 1) * m_library_page_size < m_library_entries.size());
            for (auto* thumbnail : m_library_thumbnails)
                static_cast<LibraryThumbnail*>(thumbnail)->finish_pending();
        } else {
            const size_t anchor_index = m_library_page * m_library_page_size;
            const std::string anchor = anchor_index < m_library_entries.size() ? m_library_entries[anchor_index].job_id : std::string();
            const auto position = std::find_if(snapshot->entries.begin(), snapshot->entries.end(),
                [&](const GeneratedModelEntry& entry) { return entry.job_id == anchor; });
            if (!anchor.empty() && position != snapshot->entries.end())
                m_library_page = size_t(position - snapshot->entries.begin()) / m_library_page_size;
            m_library_entries = std::move(snapshot->entries);
            refresh_library();
        }
    }
    for (const auto& thumbnail : thumbnails) {
        if (thumbnail.revision != m_library_revision || thumbnail.index >= m_library_thumbnails.size()) continue;
        wxImage image;
        if (!thumbnail.rgb.empty()) {
            if (image.Create(thumbnail.width, thumbnail.height)) {
                std::memcpy(image.GetData(), thumbnail.rgb.data(), thumbnail.rgb.size());
                if (!thumbnail.alpha.empty()) {
                    image.InitAlpha();
                    std::memcpy(image.GetAlpha(), thumbnail.alpha.data(), thumbnail.alpha.size());
                }
            }
        }
        static_cast<LibraryThumbnail*>(m_library_thumbnails[thumbnail.index])->set_image(image);
    }
    // DPI changes invalidate only the visible page; the worker cache keys
    // include pixel size, and themes repaint via the normal AI appearance path.
    if (!m_library_refresh_pending &&
        (m_library_thumbnail_edge != int(FromDIP(96) * GetContentScaleFactor()) ||
         m_library_layout_width != m_library_scroller->GetClientSize().x)) refresh_library();
}

void ModelGenerationPanel::request_library_thumbnails()
{
    cancel_library_loading();
    start_library_worker();
    m_library_refresh_pending = false;
    auto request = std::make_shared<LibraryLoadState::Request>();
    request->revision = m_library_revision;
    request->edge = m_library_thumbnail_edge;
    const auto begin = std::min(m_library_page * m_library_page_size, m_library_entries.size());
    const auto end = std::min(begin + m_library_page_size, m_library_entries.size());
    request->entries.assign(m_library_entries.begin() + begin, m_library_entries.begin() + end);
    {
        std::lock_guard<std::mutex> lock(m_library_load_state->mutex);
        m_library_load_state->pending = std::move(request);
    }
    m_library_load_state->wake.notify_one();
    m_library_timer.Start(50);
}

void ModelGenerationPanel::persist_generation_options()
{
    if (m_shutdown || m_busy || m_restoring_input || !m_awaiting_confirmation || m_job_id.empty() ||
        !job_inputs_match() || use_printable_colors() != m_job_use_printable_colors ||
        (use_printable_colors() && current_palette() != m_job_palette)) {
        refresh_controls();
        return;
    }
    if (!generation_options_valid()) {
        refresh_controls();
        show_input_hint(_L("当前设置尚未保存：200 万面需要选择精细几何。"));
        return;
    }
    const auto previous = m_job_generation_options;
    const auto requested = current_generation_options();
    const uint64_t sequence = m_sequence;
    const std::string job_id = m_job_id;
    m_saving_generation_options = true;
    m_busy = true;
    refresh_controls();
    m_status->SetLabel(_L("正在保存 3D 生成设置..."));
    wxWeakRef<ModelGenerationPanel> weak(this);
    const auto finish = [weak, sequence, job_id](AIModelGenerationClient::GenerationOptions options, wxString error) {
        if (!weak) return;
        wxGetApp().CallAfter([weak, sequence, job_id, options = std::move(options), error = std::move(error)] {
            if (!weak || weak->m_shutdown || sequence != weak->m_sequence || weak->m_job_id != job_id) return;
            weak->m_job_generation_options = options;
            weak->m_job_face_limit = options.face_limit;
            weak->m_job_generation_profile = options.face_limit <= 300000 ? "performance" : "quality";
            weak->m_provider->SetSelection(options.provider == "hunyuan" ? 1 : 0);
            weak->refresh_provider_options();
            weak->m_quality->SetSelection(options.face_limit <= 300000 ? 0 : options.face_limit == 2000000 && weak->m_quality->GetCount() == 3 ? 2 : 1);
            weak->m_geometry_quality->SetSelection(weak->m_geometry_quality->GetCount() > 1 && options.geometry_quality == "detailed" ? 1 : 0);
            weak->m_texture_quality->SetSelection(weak->m_texture_quality->GetCount() == 1 ? 0 : options.texture_quality == "extreme" ? 2 : options.texture_quality == "detailed" ? 1 : 0);
            weak->m_output_format->SetSelection(options.output_format == "obj" ? 1 : 0);
            weak->m_saving_generation_options = false;
            weak->m_busy = false;
            weak->refresh_controls();
            weak->show_input_hint(error.empty() ? _L("3D 生成设置已保存。")
                : _L("保存未确认，界面已恢复先前设置；请重新打开设计记录核对：") + error);
        });
    };
    m_client.update_generation_options(job_id, requested,
        [finish, previous, job_id](AIModelGenerationClient::JobStatus status) {
            if (status.id != job_id || status.state != "awaiting_confirmation") {
                finish(previous, _L("任务状态已变化，请重新打开设计记录。"));
                return;
            }
            finish(std::move(status.generation_options), wxString());
        },
        [finish, previous](std::string error) {
            finish(previous, wxString::FromUTF8(error));
        });
}

void ModelGenerationPanel::load_design_library_entry(const std::string& job_id)
{
    if (m_busy || m_design_history_loading || m_finishing_running || m_shutdown) return;
    if (!m_service_available || !ModelGenerationPresentation::read_design_history_entry(
            ModelGenerationPresentation::generated_models_root(), job_id)) {
        m_status->SetLabel(_L("设计记录或本地 AI 服务不可用，当前内容已保留。"));
        return;
    }
    const uint64_t sequence = m_sequence;
    const uint64_t history_sequence = ++m_design_history_sequence;
    m_design_history_loading = true;
    m_busy = true;
    refresh_controls();
    m_status->SetLabel(_L("正在恢复历史设计图..."));
    wxWeakRef<ModelGenerationPanel> weak(this);
    m_client.get_status(job_id,
        [weak, sequence, history_sequence, job_id](AIModelGenerationClient::JobStatus status) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, history_sequence, job_id, status = std::move(status)]() mutable {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence ||
                    history_sequence != weak->m_design_history_sequence) return;
                weak->m_design_history_loading = false;
                weak->m_busy = false;
                if (status.id != job_id ||
                    (status.state != "awaiting_confirmation" && status.state != "stopped" && status.state != "failed") ||
                    (!status.preview_ready && !status.raw_preview_ready && !status.model_reference_ready) ||
                    (status.state == "awaiting_confirmation" && status.source == "image" && !status.input_ready)) {
                    weak->refresh_controls();
                    weak->m_status->SetLabel(_L("设计记录不完整或状态已变化，当前内容已保留。"));
                    return;
                }
                // Reopening the current pending design after reconnect is a
                // read refresh: keep local input and unsubmitted quality choices.
                if (status.id == weak->m_job_id && weak->m_awaiting_confirmation &&
                    status.state == "awaiting_confirmation" && !weak->m_model_preview_ready && weak->m_style_preview_ready) {
                    weak->handle_status(std::move(status), sequence);
                    if (weak->m_preview_book) weak->m_preview_book->SetSelection(0);
                    return;
                }
                // Commit navigation only after the persisted job was read successfully.
                // GET and restore keep stopped/failed states and never submit generation.
                if (!weak->m_finishing_candidate.empty()) {
                    boost::system::error_code ignored;
                    boost::filesystem::remove(weak->m_finishing_candidate, ignored);
                    weak->m_finishing_candidate.clear();
                }
                weak->m_finishing_options.selected_faces.clear();
                weak->m_finishing_before = false;
                weak->m_finishing_undo_path.clear();
                weak->m_finishing_redo_path.clear();
                weak->m_selected_image_path.clear();
                weak->reset(false);
                ++weak->m_style_recommendation_sequence;
                weak->m_style_recommendation_loading = false;
                weak->m_style_recommendation_available = false;
                weak->m_style_recommendation = {};
                weak->m_history_display_image = wxImage();
                weak->m_history_display_source.clear();
                weak->set_finishing_workbench(false);
                weak->restore_job(std::move(status), weak->m_sequence);
                if (weak->m_preview_book) weak->m_preview_book->SetSelection(0);
                weak->refresh_controls();
            });
        },
        [weak, sequence, history_sequence](std::string error) {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, history_sequence, error = std::move(error)] {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence ||
                    history_sequence != weak->m_design_history_sequence) return;
                weak->m_design_history_loading = false;
                weak->m_busy = false;
                if (ModelGenerationPresentation::is_transient_sidecar_poll_error(error)) {
                    // A restarted sidecar has a new nonce. Discovery performs a
                    // fresh authenticated challenge; never replay a paid POST.
                    weak->set_service_availability(false, error);
                    weak->m_status->SetLabel(_L("服务连接已失效，当前内容已保留。\n正在重新检测，就绪后请重新打开设计。"));
                    if (weak->m_service_retry_handler) weak->m_service_retry_handler();
                    return;
                }
                weak->refresh_controls();
                weak->m_status->SetLabel(_L("历史设计加载失败，当前模型与输入已保留。"));
            });
        });
}

void ModelGenerationPanel::refresh_library()
{
    if (m_shutdown) return;
    if (m_library_sizer == nullptr || m_library_scroller == nullptr || !m_library_scroller->IsShownOnScreen()) {
        m_library_refresh_pending = true;
        return;
    }
    const int scroll_y = m_library_scroller->GetViewStart().y;
    m_library_thumbnail_edge = int(FromDIP(96) * GetContentScaleFactor());
    m_library_layout_width = m_library_scroller->GetClientSize().x;
    m_library_thumbnails.clear();
    m_library_sizer->Clear(true);
    m_library_page = std::min(m_library_page, m_library_entries.empty() ? size_t(0) : (m_library_entries.size() - 1) / m_library_page_size);
    m_library_empty->SetLabel(_L("还没有保存的设计图或模型。"));
    m_library_empty->Show(m_library_entries.empty());
    const size_t begin = m_library_page * m_library_page_size;
    const size_t end = std::min(begin + m_library_page_size, m_library_entries.size());
    for (size_t index = begin; index < end; ++index) {
        auto* card = create_library_card(m_library_entries[index]);
        refresh_ai_appearance(card);
        m_library_sizer->Add(card, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    }
    m_library_page_label->SetLabel(wxString::Format(_L("第 %llu / %llu 页 · 共 %llu 条"),
        static_cast<unsigned long long>(m_library_page + 1),
        static_cast<unsigned long long>(std::max(size_t(1), (m_library_entries.size() + m_library_page_size - 1) / m_library_page_size)),
        static_cast<unsigned long long>(m_library_entries.size())));
    m_library_previous->Enable(m_library_page > 0);
    m_library_next->Enable(end < m_library_entries.size());
    m_library_scroller->Layout();
    m_library_scroller->FitInside();
    m_library_scroller->Scroll(0, scroll_y);
    m_library_scroller->GetParent()->Layout();
    m_library_scroller->Refresh();
    request_library_thumbnails();
}

wxWindow* ModelGenerationPanel::create_library_card(const GeneratedModelEntry& entry)
{
        auto* card = new wxPanel(m_library_scroller, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        wxWindow* format = new LibraryThumbnail(card);
        m_library_thumbnails.push_back(format);
        row->Add(format, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(12));
        auto* text = new wxBoxSizer(wxVERTICAL);
        const int text_width = std::max(FromDIP(140), m_library_layout_width - FromDIP(264));
        auto* title = new wxStaticText(card, wxID_ANY, entry.title);
        wxFont title_font = title->GetFont();
        title_font.SetWeight(wxFONTWEIGHT_BOLD);
        title->SetFont(title_font);
        title->Wrap(text_width);
        text->Add(title, 0, wxBOTTOM, FromDIP(3));
        auto* details = new wxStaticText(card, wxID_ANY, entry.details);
        details->SetForegroundColour(wxColour(91, 104, 107));
        details->Wrap(text_width);
        text->Add(details, 0);
        if (!entry.provider_task_id.empty()) {
            // Use a normal button: the collapsible pane's custom header can
            // lose its caption when repainted inside the Windows model list.
            auto* diagnostics = new wxPanel(card);
            auto* diagnostics_sizer = new wxBoxSizer(wxVERTICAL);
            const bool expanded = m_library_expanded_details.count(entry.job_id) > 0;
            auto* toggle_details = new wxButton(diagnostics, wxID_ANY, expanded ? _L("收起任务详情") : _L("展开任务详情"));
            diagnostics_sizer->Add(toggle_details, 0, wxALIGN_LEFT);
            auto* task_parent = new wxPanel(diagnostics);
            auto* task_row = new wxBoxSizer(wxVERTICAL);
            auto* task_id = new wxStaticText(
                task_parent, wxID_ANY, _L("任务编号：") + wxString::FromUTF8(entry.provider_task_id));
            task_id->Wrap(text_width);
            task_id->SetForegroundColour(wxColour(31, 122, 116));
            task_id->SetToolTip(wxString::FromUTF8(entry.provider_task_id));
            auto* copy_task_id = new wxButton(
                task_parent, wxID_ANY, _L("复制"), wxDefaultPosition, wxSize(FromDIP(58), FromDIP(26)));
            copy_task_id->SetToolTip(_L("复制完整的 3D 生成任务编号"));
            copy_task_id->Bind(wxEVT_BUTTON, [this, provider_task_id = entry.provider_task_id](wxCommandEvent&) {
                bool copied = false;
                if (wxTheClipboard->Open()) {
                    copied = wxTheClipboard->SetData(
                        new wxTextDataObject(wxString::FromUTF8(provider_task_id)));
                    wxTheClipboard->Close();
                }
                m_status->SetLabel(copied ? _L("3D Task ID 已复制到剪贴板。")
                                          : _L("无法访问剪贴板，请稍后重试。"));
            });
            task_row->Add(task_id, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
            task_row->Add(copy_task_id, 0, wxALIGN_LEFT);
            task_parent->SetSizer(task_row);
            diagnostics_sizer->Add(task_parent, 0, wxEXPAND | wxTOP, FromDIP(8));
            task_parent->Show(expanded);
            diagnostics->SetSizer(diagnostics_sizer);
            text->Add(diagnostics, 0, wxEXPAND | wxTOP, FromDIP(4));
            toggle_details->Bind(wxEVT_BUTTON, [this, card, diagnostics, task_parent, toggle_details, job_id = entry.job_id](wxCommandEvent&) {
                const bool expanded = !task_parent->IsShown();
                if (expanded) m_library_expanded_details.insert(job_id);
                else m_library_expanded_details.erase(job_id);
                task_parent->Show(expanded);
                toggle_details->SetLabel(expanded ? _L("收起任务详情") : _L("展开任务详情"));
                diagnostics->InvalidateBestSize(); diagnostics->Layout();
                card->InvalidateBestSize();
                card->Layout(); m_library_scroller->Layout(); m_library_scroller->FitInside();
            });
        }
        row->Add(text, 1, wxALIGN_CENTER_VERTICAL | wxTOP | wxRIGHT | wxBOTTOM, FromDIP(8));
        auto* actions = new wxBoxSizer(wxVERTICAL);
        auto* load = new wxButton(card, wxID_ANY, entry.design_only ? _L("打开设计") : _L("加载"),
                                  wxDefaultPosition, wxSize(FromDIP(104), -1));
        load->Bind(wxEVT_BUTTON,
            [this, model_path = entry.model_path, palette = entry.palette,
              palette_roles = entry.palette_roles, use_printable_colors = entry.use_printable_colors,
              reference_image_path = entry.reference_image_path, ai_image_path = entry.ai_image_path,
              color_intent_path = entry.color_intent_path, color_intent_schema = entry.color_intent_schema,
              color_intent_sha256 = entry.color_intent_sha256,
              job_id = entry.job_id, title_text = entry.title](wxCommandEvent&) {
                load_library_entry(model_path, reference_image_path, ai_image_path, palette, palette_roles,
                                   use_printable_colors, color_intent_path, color_intent_schema,
                                   color_intent_sha256, job_id, title_text);
            });
        actions->Add(load, 0, wxEXPAND | wxBOTTOM, FromDIP(4));
        auto* reuse_geometry = new wxButton(
            card, wxID_ANY, _L("复用造型"), wxDefaultPosition, wxSize(FromDIP(104), -1));
        reuse_geometry->SetToolTip(
            _L("保留这个历史模型的网格与脸部造型，使用当前确认图片重新生成颜色"));
        reuse_geometry->Enable(
            m_service_available && !m_busy && !m_job_id.empty() && m_job_preview_expected &&
            (m_ready || m_awaiting_confirmation) && entry.job_id != m_job_id);
        reuse_geometry->Bind(wxEVT_BUTTON, [this, job_id = entry.job_id, title_text = entry.title](wxCommandEvent&) {
            on_retexture_from_library(job_id, title_text);
        });
        actions->Add(reuse_geometry, 0, wxEXPAND | wxBOTTOM, FromDIP(4));
        reuse_geometry->Show(!entry.design_only);
        auto* remove = new wxButton(card, wxID_ANY, _L("删除本地"), wxDefaultPosition, wxSize(FromDIP(104), -1));
        remove->Bind(wxEVT_BUTTON, [this, entry](wxCommandEvent&) {
            delete_library_entry(entry);
        });
        actions->Add(remove, 0, wxEXPAND | wxBOTTOM, FromDIP(4));
        if (entry.imported_at > 0) {
            const wxString feedback_label = entry.print_feedback == "success"
                ? _L("打印成功 ✓")
                : entry.print_feedback == "issue" ? _L("打印有问题") : _L("记录打印结果");
            auto* feedback = new wxButton(
                card, wxID_ANY, feedback_label, wxDefaultPosition, wxSize(FromDIP(104), -1));
            feedback->SetToolTip(_L("由测试人员记录实际打印结果；不会从打印机自动推断"));
            feedback->Bind(wxEVT_BUTTON, [this, job_id = entry.job_id](wxCommandEvent&) {
                MessageDialog dialog(
                    this,
                    _L("请根据已经完成的真实打印记录结果。\n\n“打印成功”表示成品达到本次测试预期；“有问题”表示需要后续复盘。"),
                    _L("记录实际打印结果"), wxYES_NO | wxCANCEL | wxICON_QUESTION);
                dialog.SetButtonLabel(wxID_YES, _L("打印成功"));
                dialog.SetButtonLabel(wxID_NO, _L("有问题"));
                const int result = dialog.ShowModal();
                if (result == wxID_YES)
                    record_library_print_feedback(job_id, "success");
                else if (result == wxID_NO)
                    record_library_print_feedback(job_id, "issue");
            });
            actions->Add(feedback, 0, wxEXPAND);
        }
        row->Add(actions, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxRIGHT | wxBOTTOM, FromDIP(8));
        card->SetSizer(row);
        const auto bind_load = [this, model_path = entry.model_path, palette = entry.palette,
                                 palette_roles = entry.palette_roles,
                                 use_printable_colors = entry.use_printable_colors,
                                 reference_image_path = entry.reference_image_path,
                                 ai_image_path = entry.ai_image_path,
                                 color_intent_path = entry.color_intent_path,
                                 color_intent_schema = entry.color_intent_schema,
                                 color_intent_sha256 = entry.color_intent_sha256,
                                 job_id = entry.job_id,
                                title_text = entry.title](wxWindow* window) {
            window->SetCursor(wxCursor(wxCURSOR_HAND));
            window->SetToolTip(model_path.empty() ? _L("双击打开设计图") : _L("也可双击加载到 3D 模型预览"));
            window->Bind(wxEVT_LEFT_DCLICK, [this, model_path, reference_image_path, ai_image_path,
                                             palette, palette_roles, use_printable_colors, job_id,
                                             color_intent_path, color_intent_schema, color_intent_sha256,
                                             title_text](wxMouseEvent&) {
                load_library_entry(model_path, reference_image_path, ai_image_path, palette, palette_roles,
                                   use_printable_colors, color_intent_path, color_intent_schema,
                                   color_intent_sha256, job_id, title_text);
            });
        };
        bind_load(card);
        bind_load(format);
        format->SetToolTip(entry.design_only ? _L("双击恢复设计图及其输入，不会自动生成 3D。")
                                            : _L("关联设计图缩略图；双击加载实际 3D 模型。"));
        bind_load(title);
        bind_load(details);
        return card;
}

} // namespace Slic3r::GUI
