#include <boost/filesystem/path.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/nowide/filesystem.hpp>
#include <boost/system/error_code.hpp>
#include <catch2/catch_all.hpp>

#include <cstddef>
#include <libslic3r/Utils.hpp>
#include <libslic3r/libslic3r.h>         // GCODEVIEWER_APP_KEY, SLIC3R_APP_KEY (via libslic3r_version.h)
#include <slic3r/plugin/PluginAuditManager.hpp>
#include <slic3r/plugin/PluginFsUtils.hpp>
#include <slic3r/plugin/PluginManager.hpp>
#include <slic3r/plugin/PythonInterpreter.hpp>
#include <slic3r/plugin/PythonPluginBridge.hpp>
#include <slic3r/plugin/PythonPluginInterface.hpp>
#include <slic3r/Utils/IPrinterAgent.hpp>
#include <slic3r/Utils/OrcaCloudServiceAgent.hpp> // secret_constants::USER_SECRET_FILENAME

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "libslic3r_version.h"
#include "plugin_test_utils.hpp"
#include "test_utils.hpp"

#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>
#include <pybind11/cast.h>
#include <pybind11/embed.h>
#include <pybind11/eval.h>
#include <pybind11/gil.h>
#include <pybind11/pybind11.h>
#include <pybind11/pytypes.h>
#include <pybind11/stl.h>

#include <chrono>
#include <clocale>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <functional>
#include <future>
#include <ios>
#include <iterator>
#include <locale>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

using namespace Slic3r;
namespace fs = boost::filesystem;
namespace py = pybind11;

namespace {

// Seed the deny registry with the same list install_hook() uses. Both draw from
// PluginAuditManager::default_denied_filenames(), so the test and production seeding cannot
// drift apart. The registry is a process singleton, so repeated seeding only appends harmless
// duplicates; matching is unaffected.
void seed_denied_names()
{
    PluginAuditManager& mgr = PluginAuditManager::instance();
    for (const auto& name : PluginAuditManager::default_denied_filenames())
        mgr.add_denied_filename(name);
}

// Seed the keyword registry with the same list install_hook() uses, including the narrow
// keyword exemptions install_hook() seeds right after it. Same rationale as
// seed_denied_names(): a process-singleton registry, seeded from the single shared source so
// production and tests cannot drift apart.
void seed_denied_keywords()
{
    PluginAuditManager& mgr = PluginAuditManager::instance();
    for (const auto& keyword : PluginAuditManager::default_denied_path_keywords())
        mgr.add_denied_path_keyword(keyword);
    for (const auto& name : PluginAuditManager::default_keyword_exempt_filenames())
        mgr.add_keyword_exempt_filename(name);
}

std::string read_text_file(const fs::path& path)
{
    std::ifstream in(path.string(), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

#ifdef _WIN32
// The 8.3 short form of an existing path. Empty when the volume does not generate short names
// (the conversion then returns the input unchanged) or the call fails. Whether a machine spells a
// path short is environment-dependent, so tests build the alias here instead of assuming one.
fs::path short_name_alias(const fs::path& long_path)
{
    const std::wstring native = long_path.native();
    std::wstring       buffer(native.size() + 1, L'\0');
    for (;;) {
        const DWORD length = ::GetShortPathNameW(native.c_str(), buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0)
            return {};
        if (length < buffer.size()) {
            buffer.resize(length);
            break;
        }
        buffer.resize(static_cast<std::size_t>(length) + 1);
    }
    const fs::path alias(buffer);
    return alias == long_path ? fs::path() : alias;
}
#endif

} // namespace

namespace {

AuditPromptRequest prompt_request(const std::string& plugin_key, AuditEventCategory category,
                                  std::vector<std::string> targets)
{
    AuditPromptRequest request;
    request.category    = category;
    request.plugin_key  = plugin_key;
    request.plugin_name = plugin_key;
    request.event_name  = "open";
    request.keys        = std::move(targets);
    request.shown       = request.keys;
    return request;
}

AuditPromptBucket bucket(AuditEventCategory category, std::vector<std::string> targets)
{
    return {category, std::move(targets)};
}

// A load-time declaration: one combined prompt over the buckets no earlier grant covers. A load
// worker may be blocked on its answer.
AuditPromptRequest declared_request(const std::string& plugin_key, std::vector<AuditPromptBucket> buckets)
{
    AuditPromptRequest request;
    request.kind        = AuditPromptKind::DeclaredPermissions;
    request.plugin_key  = plugin_key;
    request.plugin_name = plugin_key;
    request.buckets     = std::move(buckets);
    return request;
}

// A request plus the completion a blocked load worker waits on, and the future that worker holds.
struct WaitingRequest
{
    AuditPromptRequest                     request;
    std::shared_ptr<AuditPromptCompletion> completion;
    std::future<bool>                      future;
};

WaitingRequest waiting(AuditPromptRequest request)
{
    auto              completion = std::make_shared<AuditPromptCompletion>();
    std::future<bool> future     = completion->get_future();
    request.completion           = completion;
    return {std::move(request), std::move(completion), std::move(future)};
}

bool settled(std::future<bool>& future)
{
    return future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
}

// The drain and the unload/cancel wiring use a process-wide queue. A test that enqueues into it
// directly must leave it empty even when an assertion aborts the case.
struct ScopedRuntimePromptQueue
{
    AuditPromptQueue& queue = runtime_prompt_queue_for_testing();

    ScopedRuntimePromptQueue() { queue.discard_all(); }
    ~ScopedRuntimePromptQueue() { queue.discard_all(); }
};

// The same snapshot drain_runtime_prompts() builds for the availability check.
AuditPromptPluginState prompt_plugin_state(const std::string& plugin_key)
{
    PluginManager& manager = PluginManager::instance();
    AuditPromptPluginState state;
    state.loaded           = manager.is_plugin_loaded(plugin_key);
    state.load_in_progress = manager.is_plugin_load_in_progress(plugin_key);
    state.load_cancelled   = manager.is_plugin_load_cancelled(plugin_key);
    state.load_failed      = manager.is_plugin_load_failed(plugin_key);
    state.shutting_down    = manager.is_shutting_down();
    return state;
}

} // namespace

TEST_CASE("A runtime audit prompt is queued while another one is active", "[audit][Python]")
{
    AuditPromptQueue queue;

    CHECK(queue.admit(prompt_request("plugin", AuditEventCategory::FsRead, {"/a"}), /*may_show_now=*/true) ==
          AuditPromptQueue::Admission::ShowNow);
    // Still inside the first dialog's modal loop: a GUI-thread event must queue, not nest.
    CHECK(queue.admit(prompt_request("plugin", AuditEventCategory::FsRead, {"/b"}), /*may_show_now=*/true) ==
          AuditPromptQueue::Admission::Queued);
    // The request already on screen is not queued a second time.
    CHECK(queue.admit(prompt_request("plugin", AuditEventCategory::FsRead, {"/a"}), /*may_show_now=*/true) ==
          AuditPromptQueue::Admission::Duplicate);
    CHECK(queue.pending_count() == 1);
    CHECK_FALSE(queue.take_next().has_value());

    queue.complete();
    std::optional<AuditPromptRequest> next = queue.take_next();
    REQUIRE(next.has_value());
    CHECK(next->keys == std::vector<std::string>{"/b"});
    CHECK_FALSE(queue.take_next().has_value());
    CHECK_FALSE(queue.idle());

    queue.complete();
    CHECK(queue.idle());
}

TEST_CASE("Only exactly equivalent pending audit prompts are coalesced", "[audit][Python]")
{
    AuditPromptQueue queue;

    CHECK(queue.admit(prompt_request("plugin", AuditEventCategory::FsRead, {"/a"}), /*may_show_now=*/false) ==
          AuditPromptQueue::Admission::Queued);
    CHECK(queue.admit(prompt_request("plugin", AuditEventCategory::FsRead, {"/a"}), /*may_show_now=*/false) ==
          AuditPromptQueue::Admission::Duplicate);
    // A different category, plugin, or target is a distinct grant and never merges.
    CHECK(queue.admit(prompt_request("plugin", AuditEventCategory::FsReadWrite, {"/a"}), /*may_show_now=*/false) ==
          AuditPromptQueue::Admission::Queued);
    CHECK(queue.admit(prompt_request("other_plugin", AuditEventCategory::FsRead, {"/a"}), /*may_show_now=*/false) ==
          AuditPromptQueue::Admission::Queued);
    CHECK(queue.admit(prompt_request("plugin", AuditEventCategory::FsRead, {"/b"}), /*may_show_now=*/false) ==
          AuditPromptQueue::Admission::Queued);
    CHECK(queue.pending_count() == 4);
}

TEST_CASE("Queued audit prompts are handed out in arrival order", "[audit][Python]")
{
    AuditPromptQueue queue;

    CHECK(queue.admit(prompt_request("plugin", AuditEventCategory::FsRead, {"/a"}), /*may_show_now=*/false) ==
          AuditPromptQueue::Admission::Queued);
    CHECK(queue.admit(prompt_request("plugin", AuditEventCategory::FsRead, {"/b"}), /*may_show_now=*/false) ==
          AuditPromptQueue::Admission::Queued);

    std::optional<AuditPromptRequest> first = queue.take_next();
    REQUIRE(first.has_value());
    CHECK(first->keys == std::vector<std::string>{"/a"});
    queue.complete();

    std::optional<AuditPromptRequest> second = queue.take_next();
    REQUIRE(second.has_value());
    CHECK(second->keys == std::vector<std::string>{"/b"});
    queue.complete();
    CHECK(queue.idle());
}

TEST_CASE("Discarding drops queued audit prompts but leaves the active one", "[audit][Python]")
{
    AuditPromptQueue queue;

    CHECK(queue.admit(prompt_request("plugin", AuditEventCategory::FsRead, {"/a"}), /*may_show_now=*/true) ==
          AuditPromptQueue::Admission::ShowNow);
    CHECK(queue.admit(prompt_request("plugin", AuditEventCategory::FsRead, {"/b"}), /*may_show_now=*/false) ==
          AuditPromptQueue::Admission::Queued);

    queue.discard_all();
    CHECK_FALSE(queue.has_pending());
    queue.complete();
    CHECK_FALSE(queue.take_next().has_value());
    CHECK(queue.idle());
}

TEST_CASE("Unloading a plugin drops only that plugin's queued audit prompts", "[audit][Python]")
{
    AuditPromptQueue queue;

    CHECK(queue.admit(prompt_request("unloaded", AuditEventCategory::FsRead, {"/a"}), /*may_show_now=*/false) ==
          AuditPromptQueue::Admission::Queued);
    CHECK(queue.admit(prompt_request("kept", AuditEventCategory::FsRead, {"/b"}), /*may_show_now=*/false) ==
          AuditPromptQueue::Admission::Queued);
    CHECK(queue.admit(prompt_request("unloaded", AuditEventCategory::FsRead, {"/c"}), /*may_show_now=*/false) ==
          AuditPromptQueue::Admission::Queued);

    queue.discard_plugin("unloaded");

    std::optional<AuditPromptRequest> next = queue.take_next();
    REQUIRE(next.has_value());
    CHECK(next->plugin_key == "kept");
    queue.complete();
    CHECK(queue.idle());
}

TEST_CASE("Runtime and declared audit prompts are serialized in arrival order", "[audit][Python]")
{
    AuditPromptQueue queue;

    // A GUI-thread event arrives first and is on screen; the declaration and the create/delete
    // runtime prompt that follow both have to wait for it.
    CHECK(queue.admit(prompt_request("plugin", AuditEventCategory::FsRead, {"/runtime"}), /*may_show_now=*/true) ==
          AuditPromptQueue::Admission::ShowNow);
    CHECK(queue.admit(declared_request("plugin", {bucket(AuditEventCategory::Http, {"https://api.example.com"})}),
                      /*may_show_now=*/true) == AuditPromptQueue::Admission::Queued);
    AuditPromptRequest create = prompt_request("plugin", AuditEventCategory::FsCreate, {"/work"});
    create.entries            = {{"/work/new.txt", "/work"}};
    CHECK(queue.admit(create, /*may_show_now=*/true) == AuditPromptQueue::Admission::Queued);
    CHECK(queue.pending_count() == 2);

    // At most one prompt is on screen: nothing is handed out until the active dialog returns.
    CHECK_FALSE(queue.take_next().has_value());

    queue.complete();
    std::optional<AuditPromptRequest> declared = queue.take_next();
    REQUIRE(declared.has_value());
    CHECK(declared->kind == AuditPromptKind::DeclaredPermissions);
    REQUIRE(declared->buckets.size() == 1);
    CHECK(declared->buckets.front().category == AuditEventCategory::Http);
    const std::vector<std::string> declared_targets{"https://api.example.com"};
    CHECK(declared->buckets.front().targets == declared_targets);
    // The declaration's dialog is up now: the runtime prompt still waits behind it.
    CHECK_FALSE(queue.take_next().has_value());
    CHECK(queue.pending_count() == 1);
    queue.complete();

    std::optional<AuditPromptRequest> queued_create = queue.take_next();
    REQUIRE(queued_create.has_value());
    CHECK(queued_create->kind == AuditPromptKind::RuntimeEvent);
    CHECK(queued_create->category == AuditEventCategory::FsCreate);
    // A queued create/delete prompt keeps both the folder a Yes saves and the triggering path the
    // dialog shows.
    const std::vector<std::string> saved_folders{"/work"};
    CHECK(queued_create->keys == saved_folders);
    CHECK(queued_create->entries ==
          std::vector<PluginAuditManager::PermissionPromptEntry>{{"/work/new.txt", "/work"}});
    CHECK(queued_create->completion == nullptr);
    queue.complete();
    CHECK(queue.idle());
}

TEST_CASE("Only exactly equivalent declared permission prompts are coalesced", "[audit][Python]")
{
    AuditPromptQueue queue;

    const AuditPromptRequest declaration = declared_request("plugin", {bucket(AuditEventCategory::FsRead, {"/a"})});
    CHECK(queue.admit(declaration, /*may_show_now=*/false) == AuditPromptQueue::Admission::Queued);
    CHECK(queue.admit(declaration, /*may_show_now=*/false) == AuditPromptQueue::Admission::Duplicate);
    // Same targets for another plugin is a different grant.
    CHECK(queue.admit(declared_request("other_plugin", {bucket(AuditEventCategory::FsRead, {"/a"})}),
                      /*may_show_now=*/false) == AuditPromptQueue::Admission::Queued);
    // The same plugin asking for another category or another target is a different grant.
    CHECK(queue.admit(declared_request("plugin", {bucket(AuditEventCategory::FsReadWrite, {"/a"})}),
                      /*may_show_now=*/false) == AuditPromptQueue::Admission::Queued);
    CHECK(queue.admit(declared_request("plugin", {bucket(AuditEventCategory::FsRead, {"/b"})}),
                      /*may_show_now=*/false) == AuditPromptQueue::Admission::Queued);
    CHECK(queue.pending_count() == 4);

    // The declaration already on screen is not queued a second time either.
    AuditPromptQueue shown;
    CHECK(shown.admit(declaration, /*may_show_now=*/true) == AuditPromptQueue::Admission::ShowNow);
    CHECK(shown.admit(declaration, /*may_show_now=*/true) == AuditPromptQueue::Admission::Duplicate);
    CHECK(shown.pending_count() == 0);
    CHECK_FALSE(shown.idle());
}

TEST_CASE("Queued create/delete prompts are coalesced only when their triggering paths match", "[audit][Python]")
{
    AuditPromptQueue queue;

    AuditPromptRequest first = prompt_request("plugin", AuditEventCategory::FsCreate, {"/work"});
    first.entries            = {{"/work/new.txt", "/work"}};
    CHECK(queue.admit(first, /*may_show_now=*/false) == AuditPromptQueue::Admission::Queued);
    CHECK(queue.admit(first, /*may_show_now=*/false) == AuditPromptQueue::Admission::Duplicate);

    // The same saved folder reached through another path is a different prompt: it shows a
    // different target, so dropping it would hide why the plugin was stopped.
    AuditPromptRequest second = first;
    second.entries            = {{"/work/other.txt", "/work"}};
    CHECK(queue.admit(second, /*may_show_now=*/false) == AuditPromptQueue::Admission::Queued);

    // Another category, another saved folder, or another plugin is a separate prompt too.
    AuditPromptRequest deleted = prompt_request("plugin", AuditEventCategory::FsDelete, {"/work"});
    deleted.entries            = {{"/work/old.txt", "/work"}};
    CHECK(queue.admit(deleted, /*may_show_now=*/false) == AuditPromptQueue::Admission::Queued);
    AuditPromptRequest other_plugin = first;
    other_plugin.plugin_key         = "other_plugin";
    CHECK(queue.admit(other_plugin, /*may_show_now=*/false) == AuditPromptQueue::Admission::Queued);
    CHECK(queue.pending_count() == 4);
}

TEST_CASE("A queued create/delete prompt keeps the target entries and the folder a Yes saves", "[audit][Python]")
{
    AuditPromptQueue queue;

    // Another dialog is up, so the create prompt waits rather than opening a second modal. Its
    // approval still saves the folder, so a retry can succeed.
    CHECK(queue.admit(prompt_request("plugin", AuditEventCategory::FsRead, {"/busy"}), /*may_show_now=*/true) ==
          AuditPromptQueue::Admission::ShowNow);
    AuditPromptRequest create = prompt_request("plugin", AuditEventCategory::FsCreate, {"/work"});
    create.entries            = {{"/work/new.txt", "/work"}};
    CHECK(queue.admit(create, /*may_show_now=*/true) == AuditPromptQueue::Admission::Queued);
    CHECK(queue.pending_count() == 1);
    queue.complete();

    std::optional<AuditPromptRequest> queued = queue.take_next();
    REQUIRE(queued.has_value());
    CHECK(queued->kind == AuditPromptKind::RuntimeEvent);
    CHECK(queued->category == AuditEventCategory::FsCreate);
    const std::vector<std::string> expected_folder{"/work"};
    CHECK(queued->keys == expected_folder);
    CHECK(queued->entries ==
          std::vector<PluginAuditManager::PermissionPromptEntry>{{"/work/new.txt", "/work"}});
    CHECK(queued->completion == nullptr);
    queue.complete();
    CHECK(queue.idle());

    // A worker create also keeps its Yes/No prompt while the queue is idle; the current operation
    // stays denied, but the queued answer persists the folder for a retry.
    AuditPromptQueue worker;
    CHECK(worker.admit(create, /*may_show_now=*/false) == AuditPromptQueue::Admission::Queued);
    std::optional<AuditPromptRequest> retry = worker.take_next();
    REQUIRE(retry.has_value());
    CHECK(retry->kind == AuditPromptKind::RuntimeEvent);
    CHECK(retry->keys == expected_folder);
    CHECK(retry->entries ==
          std::vector<PluginAuditManager::PermissionPromptEntry>{{"/work/new.txt", "/work"}});
    worker.complete();
    CHECK(worker.idle());
}

TEST_CASE("A recursive-delete consent shares the serialized audit prompt queue", "[audit][Python]")
{
    AuditPromptQueue queue;

    AuditPromptRequest consent;
    consent.kind                        = AuditPromptKind::RecursiveDeleteConsent;
    consent.plugin_key                  = "plugin";
    consent.plugin_name                 = "Plugin";
    consent.event_name                  = "shutil.rmtree";
    consent.shown                       = {"/work/sub"};
    consent.root_scope                  = false;
    consent.recursive_delete_generation = 7;

    // A permission dialog is on screen: the consent waits and is not duplicated by a retry loop.
    CHECK(queue.admit(prompt_request("plugin", AuditEventCategory::FsRead, {"/busy"}), /*may_show_now=*/true) ==
          AuditPromptQueue::Admission::ShowNow);
    CHECK(queue.admit(consent, /*may_show_now=*/true) == AuditPromptQueue::Admission::Queued);
    CHECK(queue.admit(consent, /*may_show_now=*/true) == AuditPromptQueue::Admission::Duplicate);
    CHECK(queue.pending_count() == 1);
    queue.complete();

    std::optional<AuditPromptRequest> queued = queue.take_next();
    REQUIRE(queued.has_value());
    CHECK(queued->kind == AuditPromptKind::RecursiveDeleteConsent);
    const std::vector<std::string> target{"/work/sub"};
    CHECK(queued->shown == target);
    CHECK(queued->root_scope == false);
    CHECK(queued->recursive_delete_generation == 7);
    queue.complete();
    CHECK(queue.idle());
}

TEST_CASE("Discarding a plugin drops its queued recursive-delete consent", "[audit][Python]")
{
    AuditPromptQueue queue;

    AuditPromptRequest consent;
    consent.kind       = AuditPromptKind::RecursiveDeleteConsent;
    consent.plugin_key = "unloaded";
    consent.shown      = {"/work/sub"};
    CHECK(queue.admit(consent, /*may_show_now=*/false) == AuditPromptQueue::Admission::Queued);

    queue.discard_plugin("unloaded");
    CHECK_FALSE(queue.has_pending());
    queue.complete();
    CHECK_FALSE(queue.take_next().has_value());
    CHECK(queue.idle());
}

TEST_CASE("A permission change drops only that plugin's queued recursive-delete consents", "[audit][Python]")
{
    AuditPromptQueue queue;

    AuditPromptRequest consent;
    consent.kind       = AuditPromptKind::RecursiveDeleteConsent;
    consent.plugin_key = "plugin";
    consent.shown      = {"/work/sub"};
    CHECK(queue.admit(consent, /*may_show_now=*/false) == AuditPromptQueue::Admission::Queued);
    CHECK(queue.admit(prompt_request("plugin", AuditEventCategory::FsRead, {"/still-needed"}), /*may_show_now=*/false) ==
          AuditPromptQueue::Admission::Queued);

    queue.discard_recursive_delete_consents("plugin");
    CHECK(queue.pending_count() == 1);

    std::optional<AuditPromptRequest> kept = queue.take_next();
    REQUIRE(kept.has_value());
    CHECK(kept->kind == AuditPromptKind::RuntimeEvent);
    queue.complete();
    CHECK(queue.idle());

    // An active consent cannot be withdrawn, but it is marked cancelled so its answer stores
    // nothing.
    CHECK(queue.admit(consent, /*may_show_now=*/true) == AuditPromptQueue::Admission::ShowNow);
    queue.discard_recursive_delete_consents("plugin");
    CHECK(queue.complete());
    CHECK(queue.idle());
}

TEST_CASE("A recursive-delete retry after a permission change is not coalesced into the stale consent", "[audit][Python]")
{
    AuditPromptQueue queue;

    AuditPromptRequest stale;
    stale.kind                        = AuditPromptKind::RecursiveDeleteConsent;
    stale.plugin_key                  = "plugin";
    stale.shown                       = {"/work/sub"};
    stale.recursive_delete_generation = 1;
    CHECK(queue.admit(stale, /*may_show_now=*/true) == AuditPromptQueue::Admission::ShowNow);

    // The permission edit bumped the generation while the old dialog was up. The retry must not
    // be dropped as a duplicate of a prompt whose Yes can no longer store anything.
    AuditPromptRequest retry = stale;
    retry.recursive_delete_generation = 2;
    CHECK(queue.admit(retry, /*may_show_now=*/true) == AuditPromptQueue::Admission::Queued);
    CHECK(queue.pending_count() == 1);

    queue.complete();
    std::optional<AuditPromptRequest> fresh = queue.take_next();
    REQUIRE(fresh.has_value());
    CHECK(fresh->kind == AuditPromptKind::RecursiveDeleteConsent);
    CHECK(fresh->recursive_delete_generation == 2);
    queue.complete();
    CHECK(queue.idle());
}

TEST_CASE("A permission invalidation drops queued recursive-delete consents", "[audit][Python]")
{
    ScopedRuntimePromptQueue queue;

    AuditPromptRequest consent;
    consent.kind                        = AuditPromptKind::RecursiveDeleteConsent;
    consent.plugin_key                  = "queue-invalidation-test";
    consent.shown                       = {"/work/sub"};
    consent.recursive_delete_generation = PluginAuditManager::recursive_delete_generation_for_testing(consent.plugin_key);
    CHECK(queue.queue.admit(consent, /*may_show_now=*/false) == AuditPromptQueue::Admission::Queued);

    PluginAuditManager::instance().invalidate_recursive_delete_state(consent.plugin_key);
    CHECK_FALSE(queue.queue.has_pending());

    // A retry now carries the new generation and is accepted as a fresh consent.
    consent.recursive_delete_generation = PluginAuditManager::recursive_delete_generation_for_testing(consent.plugin_key);
    CHECK(queue.queue.admit(consent, /*may_show_now=*/false) == AuditPromptQueue::Admission::Queued);
}

TEST_CASE("The serialized queue bounds pending recursive-delete consents", "[audit][Python]")
{
    AuditPromptQueue queue;

    AuditPromptRequest consent;
    consent.kind       = AuditPromptKind::RecursiveDeleteConsent;
    consent.plugin_key = "plugin";
    for (int i = 0; i < 16; ++i) {
        consent.shown = {"/work/" + std::to_string(i)};
        CHECK(queue.admit(consent, /*may_show_now=*/false) == AuditPromptQueue::Admission::Queued);
    }
    // The bound is the same as the direct-prompt path: a new distinct target is rejected, while
    // the request already queued is still recognised as a duplicate.
    consent.shown = {"/work/extra"};
    CHECK(queue.admit(consent, /*may_show_now=*/false) == AuditPromptQueue::Admission::Rejected);
    consent.shown = {"/work/0"};
    CHECK(queue.admit(consent, /*may_show_now=*/false) == AuditPromptQueue::Admission::Duplicate);
    CHECK(queue.pending_count() == 16);
}

TEST_CASE("A synchronous audit prompt is shown only while the queue is idle", "[audit][Python]")
{
    AuditPromptQueue queue;

    // Idle: the prompt is shown now, and nothing is left for a later GUI callback.
    CHECK(queue.admit_synchronous(prompt_request("plugin", AuditEventCategory::FsRead, {"/a"})) ==
          AuditPromptQueue::Admission::ShowNow);
    CHECK(queue.pending_count() == 0);
    CHECK_FALSE(queue.take_next().has_value());
    queue.complete();
    CHECK(queue.idle());

    // Another prompt is on screen: the synchronous caller is rejected, whether its request is
    // equivalent to the active one or not, and no deferred copy is stored.
    CHECK(queue.admit_synchronous(prompt_request("plugin", AuditEventCategory::FsRead, {"/a"})) ==
          AuditPromptQueue::Admission::ShowNow);
    CHECK(queue.admit_synchronous(prompt_request("plugin", AuditEventCategory::FsRead, {"/a"})) ==
          AuditPromptQueue::Admission::Rejected);
    CHECK(queue.admit_synchronous(prompt_request("plugin", AuditEventCategory::FsRead, {"/b"})) ==
          AuditPromptQueue::Admission::Rejected);
    CHECK(queue.pending_count() == 0);
    CHECK_FALSE(queue.take_next().has_value());
    // The rejection did not cancel or otherwise disturb the active prompt.
    CHECK_FALSE(queue.complete());
    CHECK(queue.idle());

    // A prompt already waiting counts as busy too: the rejection leaves the pending count and the
    // queued request untouched, and nothing is deferred behind it.
    CHECK(queue.admit(prompt_request("plugin", AuditEventCategory::FsRead, {"/queued"}), /*may_show_now=*/false) ==
          AuditPromptQueue::Admission::Queued);
    CHECK(queue.admit_synchronous(prompt_request("plugin", AuditEventCategory::FsRead, {"/rejected"})) ==
          AuditPromptQueue::Admission::Rejected);
    CHECK(queue.pending_count() == 1);

    std::optional<AuditPromptRequest> queued = queue.take_next();
    REQUIRE(queued.has_value());
    const std::vector<std::string> expected{"/queued"};
    CHECK(queued->keys == expected);
    queue.complete();
    CHECK(queue.idle());
    CHECK_FALSE(queue.take_next().has_value());
}

TEST_CASE("Discarding all audit prompts denies every blocked waiter", "[audit][Python]")
{
    AuditPromptQueue queue;

    WaitingRequest active = waiting(prompt_request("plugin", AuditEventCategory::FsRead, {"/active"}));
    CHECK(queue.admit(active.request, /*may_show_now=*/true) == AuditPromptQueue::Admission::ShowNow);

    WaitingRequest queued_runtime = waiting(prompt_request("plugin", AuditEventCategory::FsRead, {"/queued"}));
    WaitingRequest queued_load =
        waiting(declared_request("plugin", {bucket(AuditEventCategory::Http, {"https://api.example.com"})}));
    CHECK(queue.admit(queued_runtime.request, /*may_show_now=*/false) == AuditPromptQueue::Admission::Queued);
    CHECK(queue.admit(queued_load.request, /*may_show_now=*/false) == AuditPromptQueue::Admission::Queued);

    queue.discard_all();
    // A late Yes from the dialog that was still up at shutdown never becomes an approval.
    active.completion->resolve(true);

    REQUIRE(settled(active.future));
    REQUIRE(settled(queued_runtime.future));
    REQUIRE(settled(queued_load.future));
    CHECK(active.future.get() == false);
    CHECK(queued_runtime.future.get() == false);
    CHECK(queued_load.future.get() == false);
    CHECK_FALSE(queue.has_pending());

    // The cancelled dialog stays on screen, so a new prompt queues instead of nesting under it.
    CHECK_FALSE(queue.take_next().has_value());
    CHECK(queue.admit(prompt_request("other", AuditEventCategory::FsRead, {"/new"}), /*may_show_now=*/true) ==
          AuditPromptQueue::Admission::Queued);
    CHECK(queue.complete());
    CHECK_FALSE(queue.complete());
    CHECK_FALSE(queue.idle());

    std::optional<AuditPromptRequest> next = queue.take_next();
    REQUIRE(next.has_value());
    const std::vector<std::string> expected{"/new"};
    CHECK(next->keys == expected);
    queue.complete();
    CHECK(queue.idle());
}

TEST_CASE("Unloading a plugin denies its blocked waiters and keeps other plugins' prompts", "[audit][Python]")
{
    AuditPromptQueue queue;

    WaitingRequest unloaded = waiting(prompt_request("unloaded", AuditEventCategory::FsRead, {"/a"}));
    WaitingRequest kept = waiting(declared_request("kept", {bucket(AuditEventCategory::FsRead, {"/b"})}));
    CHECK(queue.admit(unloaded.request, /*may_show_now=*/false) == AuditPromptQueue::Admission::Queued);
    CHECK(queue.admit(kept.request, /*may_show_now=*/false) == AuditPromptQueue::Admission::Queued);

    queue.discard_plugin("absent");
    CHECK(queue.pending_count() == 2);

    queue.discard_plugin("unloaded");

    REQUIRE(settled(unloaded.future));
    CHECK(unloaded.future.get() == false);
    // The surviving load waiter is not released by another plugin's unload.
    CHECK_FALSE(settled(kept.future));
    CHECK(queue.pending_count() == 1);

    std::optional<AuditPromptRequest> next = queue.take_next();
    REQUIRE(next.has_value());
    CHECK(next->plugin_key == "kept");
    CHECK(next->kind == AuditPromptKind::DeclaredPermissions);
    CHECK(next->completion == kept.completion);
    queue.complete();
    kept.completion->resolve(true);
    REQUIRE(settled(kept.future));
    CHECK(kept.future.get() == true);
    CHECK(queue.idle());
}

TEST_CASE("A committed answer is not cancelled by a discard that races it", "[audit][Python]")
{
    AuditPromptQueue queue;
    CHECK_FALSE(queue.begin_commit()); // nothing active to commit

    WaitingRequest active = waiting(declared_request("plugin", {bucket(AuditEventCategory::FsRead, {"/a"})}));
    CHECK(queue.admit(active.request, /*may_show_now=*/true) == AuditPromptQueue::Admission::ShowNow);

    // The user answered and the GUI thread is about to write the sidecar. An unload racing that
    // write must neither deny the worker nor forbid the grant it approved.
    CHECK(queue.begin_commit());
    queue.discard_plugin("plugin");
    CHECK_FALSE(settled(active.future));
    CHECK_FALSE(queue.complete());

    // A shutdown that wins the race before the commit does deny the waiter and forbids the write.
    WaitingRequest late = waiting(declared_request("other", {bucket(AuditEventCategory::FsRead, {"/b"})}));
    CHECK(queue.admit(late.request, /*may_show_now=*/true) == AuditPromptQueue::Admission::ShowNow);
    queue.discard_all();
    CHECK_FALSE(queue.begin_commit());
    REQUIRE(settled(late.future));
    CHECK(late.future.get() == false);
    CHECK(queue.complete());
    CHECK(queue.idle());
}

TEST_CASE("Unloading a plugin cancels its active prompt but leaves it on screen", "[audit][Python]")
{
    AuditPromptQueue queue;

    WaitingRequest unloaded = waiting(prompt_request("unloaded", AuditEventCategory::FsRead, {"/a"}));
    WaitingRequest kept = waiting(declared_request("kept", {bucket(AuditEventCategory::FsRead, {"/b"})}));
    CHECK(queue.admit(unloaded.request, /*may_show_now=*/true) == AuditPromptQueue::Admission::ShowNow);
    CHECK(queue.admit(kept.request, /*may_show_now=*/false) == AuditPromptQueue::Admission::Queued);

    queue.discard_plugin("unloaded");
    // The dialog returns after the unload; the late answer must not approve the load worker.
    unloaded.completion->resolve(true);

    REQUIRE(settled(unloaded.future));
    CHECK(unloaded.future.get() == false);
    CHECK_FALSE(queue.take_next().has_value());
    // complete() reports the cancellation, so the caller must not persist the accepted grant.
    CHECK(queue.complete());
    CHECK_FALSE(settled(kept.future));

    std::optional<AuditPromptRequest> next = queue.take_next();
    REQUIRE(next.has_value());
    CHECK(next->plugin_key == "kept");
    queue.complete();
    CHECK(queue.idle());
}

TEST_CASE("A queued runtime prompt survives an ordinary load failure", "[audit]")
{
    // The load failed after its plugin code raised the event: the event's denial may be why it
    // failed, and approving the saved grant is what lets the user retry the load.
    AuditPromptPluginState failed;
    failed.load_failed = true;
    CHECK(prompt_plugin_is_available(failed, AuditPromptKind::RuntimeEvent));

    // A declaration is answered by the load worker that is loading; once no load is in progress,
    // nothing waits on it. A one-time recursive-delete consent is a runtime prompt: its denial
    // may be what aborted the load, so an ordinary failure keeps it actionable too.
    CHECK_FALSE(prompt_plugin_is_available(failed, AuditPromptKind::DeclaredPermissions));
    CHECK(prompt_plugin_is_available(failed, AuditPromptKind::RecursiveDeleteConsent));

    // Explicit cancellation and unload discard the prompt instead: the queue removes its entry
    // and a request admitted after that discard must not become a stale dialog.
    AuditPromptPluginState cancelled = failed;
    cancelled.load_cancelled         = true;
    CHECK_FALSE(prompt_plugin_is_available(cancelled, AuditPromptKind::RuntimeEvent));

    AuditPromptPluginState unloaded = failed;
    unloaded.load_failed            = false;
    CHECK_FALSE(prompt_plugin_is_available(unloaded, AuditPromptKind::RuntimeEvent));

    AuditPromptPluginState shutting_down = failed;
    shutting_down.shutting_down          = true;
    CHECK_FALSE(prompt_plugin_is_available(shutting_down, AuditPromptKind::RuntimeEvent));

    // The normal paths still decide prompts for a live plugin.
    AuditPromptPluginState loaded;
    loaded.loaded = true;
    CHECK(prompt_plugin_is_available(loaded, AuditPromptKind::DeclaredPermissions));
    AuditPromptPluginState loading;
    loading.load_in_progress = true;
    CHECK(prompt_plugin_is_available(loading, AuditPromptKind::RuntimeEvent));
}

TEST_CASE("Plugin audit denies host-managed filenames anywhere", "[audit]")
{
    seed_denied_names();
    const PluginAuditManager& mgr = PluginAuditManager::instance();

    SECTION("the seeded names are denied by their base name")
    {
        CHECK(mgr.is_denied_filename(fs::path(SLIC3R_APP_KEY ".conf")));
        CHECK(mgr.is_denied_filename(fs::path(GCODEVIEWER_APP_KEY ".conf")));
        CHECK(mgr.is_denied_filename(fs::path(SLIC3R_APP_KEY ".ini")));
        CHECK(mgr.is_denied_filename(fs::path(GCODEVIEWER_APP_KEY ".ini")));
        CHECK(mgr.is_denied_filename(fs::path(secret_constants::USER_SECRET_FILENAME)));
        CHECK(mgr.is_denied_filename(fs::path(".install_state.json")));
    }

    SECTION("companions holding the same secrets are denied by the prefix rule")
    {
        CHECK(mgr.is_denied_filename(fs::path(SLIC3R_APP_KEY ".conf.bak")));
        CHECK(mgr.is_denied_filename(fs::path(std::string(secret_constants::USER_SECRET_FILENAME) + ".tmp")));
        // Windows alternate data streams share the same base name.
        CHECK(mgr.is_denied_filename(fs::path(SLIC3R_APP_KEY ".conf:stream")));
    }

    SECTION("the denial ignores the directory the file lives in")
    {
        CHECK(mgr.is_denied_filename(fs::path("/tmp") / (SLIC3R_APP_KEY ".conf")));
        CHECK(mgr.is_denied_filename(fs::path("/some/plugin/dir") / (SLIC3R_APP_KEY ".conf")));
        // Traversal is handled for free: filename() of the path below is already the denied name.
        CHECK(mgr.is_denied_filename(fs::path(data_dir()) / "plugins" / ".." / (SLIC3R_APP_KEY ".conf")));
    }

    SECTION("matching is case-insensitive on every platform")
    {
        CHECK(mgr.is_denied_filename(fs::path("orcaslicer.conf")));
        CHECK(mgr.is_denied_filename(fs::path("ORCASLICER.CONF")));
        CHECK(mgr.is_denied_filename(fs::path("ORCA_REFRESH_TOKEN.SEC")));
    }

    SECTION("an unrelated name that merely shares a stem is not denied")
    {
        // The prefix is the full registered name ("OrcaSlicer.conf"), not the stem "OrcaSlicer",
        // so a sibling file with a different extension/suffix stays allowed.
        CHECK_FALSE(mgr.is_denied_filename(fs::path(data_dir()) / (SLIC3R_APP_KEY "_other.txt")));
        CHECK_FALSE(mgr.is_denied_filename(fs::path(data_dir()) / (SLIC3R_APP_KEY ".json")));
        CHECK_FALSE(mgr.is_denied_filename(fs::path("orca_refresh_token.txt")));
    }

    SECTION("an empty path is not denied")
    {
        CHECK_FALSE(mgr.is_denied_filename(fs::path()));
    }
}

TEST_CASE("Plugin audit deny beats allowed roots", "[audit]")
{
    ScopedDataDir data_dir_guard("plugin-audit-deny");
    seed_denied_names();

    PluginAuditManager& mgr = PluginAuditManager::instance();
    // Reproduce install_hook()'s grant: data_dir() is a global allowed root, so both the app
    // config and the token would otherwise be reachable simply by living inside it.
    mgr.add_global_allowed_root(data_dir());

    // Enter a plugin context. The deny must hold even inside a globally allowed root.
    ScopedPluginAuditContext ctx("test_plugin", "");

    const fs::path conf  = fs::path(data_dir()) / (SLIC3R_APP_KEY ".conf");
    const fs::path token = fs::path(data_dir()) / secret_constants::USER_SECRET_FILENAME;
    const fs::path install_state = fs::path(data_dir()) / "plugins" / "example" / ".install_state.json";

    SECTION("a non-denied file inside the allowed root is writable (root really grants writes)")
    {
        AuditDecision decision = mgr.check_open((fs::path(data_dir()) / "plugin_data.txt").string(), "w");
        CHECK(decision.allowed);
    }

    SECTION("a file outside the allowed root is blocked for reads as well as writes")
    {
        AuditDecision decision = mgr.check_open((fs::path(data_dir()).parent_path() / "outside.txt").string(), "r");
        CHECK_FALSE(decision.allowed);
        CHECK(decision.reason == "outside allowed root");
    }

    SECTION("writing the app config is blocked despite data_dir() being allowed")
    {
        AuditDecision decision = mgr.check_open(conf.string(), "w");
        CHECK_FALSE(decision.allowed);
        CHECK(decision.reason == "denied filename");
    }

    SECTION("reading the app config is blocked despite the allowed root")
    {
        AuditDecision decision = mgr.check_open(conf.string(), "r");
        CHECK_FALSE(decision.allowed);
        CHECK(decision.reason == "denied filename");
    }

    SECTION("reading the cloud refresh token is blocked")
    {
        AuditDecision decision = mgr.check_open(token.string(), "r");
        CHECK_FALSE(decision.allowed);
    }

    SECTION("the token staging companion (.tmp) is blocked too")
    {
        AuditDecision decision = mgr.check_open((token.string() + ".tmp"), "w");
        CHECK_FALSE(decision.allowed);
    }

    SECTION("plugin install state is blocked for reads and writes despite the allowed root")
    {
        AuditDecision read_decision = mgr.check_open(install_state.string(), "r");
        CHECK_FALSE(read_decision.allowed);
        CHECK(read_decision.reason == "denied filename");

        AuditDecision write_decision = mgr.check_open(install_state.string(), "w");
        CHECK_FALSE(write_decision.allowed);
        CHECK(write_decision.reason == "denied filename");
    }

    SECTION("a traversal path resolving to the config is blocked")
    {
        const fs::path traversal = fs::path(data_dir()) / "plugins" / ".." / (SLIC3R_APP_KEY ".conf");
        AuditDecision  decision  = mgr.check_open(traversal.string(), "r");
        CHECK_FALSE(decision.allowed);
    }
}

TEST_CASE("Plugin audit deny beats a plugin's own scoped root", "[audit]")
{
    ScopedDataDir data_dir_guard("plugin-audit-scoped");
    seed_denied_names();

    PluginAuditManager& mgr = PluginAuditManager::instance();

    // A plugin's private directory, granted as a scoped root while it runs.
    const fs::path plugin_dir = fs::path(data_dir()) / "plugins" / "test_plugin";
    fs::create_directories(plugin_dir);

    ScopedPluginAuditContext ctx("test_plugin", "");
    mgr.add_scoped_allowed_root(plugin_dir);

    SECTION("the plugin's own non-denied file opens for read and write")
    {
        const std::string own_file = (plugin_dir / "state.json").string();
        CHECK(mgr.check_open(own_file, "r").allowed);
        CHECK(mgr.check_open(own_file, "w").allowed);
    }

    SECTION("a denied name stashed inside the plugin's own root is still blocked")
    {
        const std::string smuggled = (plugin_dir / (SLIC3R_APP_KEY ".conf")).string();
        AuditDecision      decision = mgr.check_open(smuggled, "w");
        CHECK_FALSE(decision.allowed);
        CHECK(decision.reason == "denied filename");
    }
}

TEST_CASE("Plugin audit does not constrain non-plugin code", "[audit]")
{
    ScopedDataDir data_dir_guard("plugin-audit-noplugin");
    seed_denied_names();

    PluginAuditManager& mgr = PluginAuditManager::instance();
    mgr.clear_current_plugin(); // no plugin context: this is OrcaSlicer's own C++/internal Python

    const fs::path conf = fs::path(data_dir()) / (SLIC3R_APP_KEY ".conf");

    // The name is still recognised as denied...
    CHECK(mgr.is_denied_filename(conf));
    // ...but with no current plugin the access check allows it: denies constrain plugin code only.
    CHECK(mgr.check_open(conf.string(), "w").allowed);
    CHECK(mgr.check_open(conf.string(), "r").allowed);
}

TEST_CASE("Plugin audit denies secret/certificate/config-like paths by keyword", "[audit]")
{
    seed_denied_keywords();
    const PluginAuditManager& mgr = PluginAuditManager::instance();

    SECTION("a 'secrets' directory component is denied")
    {
        CHECK(mgr.is_denied_path_keyword(fs::path("/plugin/secrets/api_key.json")));
        CHECK(mgr.is_denied_path_keyword(fs::path("/plugin/secret/token.txt")));
    }

    SECTION("a 'certificate(s)' directory component is denied")
    {
        CHECK(mgr.is_denied_path_keyword(fs::path("/resources/cert/slicer_base64.cer")));
        CHECK(mgr.is_denied_path_keyword(fs::path("/resources/certificates/ca.pem")));
    }

    SECTION("a 'conf'/'config' directory or config file component is denied")
    {
        CHECK(mgr.is_denied_path_keyword(fs::path("/plugin/conf/settings.json")));
        CHECK(mgr.is_denied_path_keyword(fs::path("/plugin/config/settings.json")));
        CHECK(mgr.is_denied_path_keyword(fs::path("/plugin/plugin.conf")));
        CHECK(mgr.is_denied_path_keyword(fs::path("/plugin/plugin.ini")));
    }

    SECTION("matching is case-insensitive")
    {
        CHECK(mgr.is_denied_path_keyword(fs::path("/plugin/SECRETS/token.txt")));
        CHECK(mgr.is_denied_path_keyword(fs::path("/resources/Certificates/ca.pem")));
        CHECK(mgr.is_denied_path_keyword(fs::path("/plugin/PLUGIN.CONF")));
    }

    SECTION("matching is not limited to the base name -- any ancestor component counts")
    {
        CHECK(mgr.is_denied_path_keyword(fs::path("/data/secrets/nested/deep/file.txt")));
    }

    SECTION("an unrelated path is not denied")
    {
        CHECK_FALSE(mgr.is_denied_path_keyword(fs::path("/plugin/output/model.gcode")));
        CHECK_FALSE(mgr.is_denied_path_keyword(fs::path("/plugin/storage/state.json")));
        CHECK_FALSE(mgr.is_denied_path_keyword(fs::path("/python/packages/cp312/numpy/__config__.py")));
        CHECK_FALSE(mgr.is_denied_path_keyword(fs::path("/python/packages/cp312/numpy/_core/_ufunc_config.py")));
        CHECK_FALSE(mgr.is_denied_path_keyword(fs::path("/python/Lib/configparser.py")));
        CHECK_FALSE(mgr.is_denied_path_keyword(fs::path("/python/Lib/sysconfig.py")));
        CHECK_FALSE(mgr.is_denied_path_keyword(fs::path("/python/Lib/logging/config.py")));
        CHECK_FALSE(mgr.is_denied_path_keyword(fs::path("/python/packages/cp312/certifi/cacert.pem")));
        CHECK_FALSE(mgr.is_denied_path_keyword(fs::path("/users/Conference/output.txt")));
        CHECK_FALSE(mgr.is_denied_path_keyword(fs::path("/users/Concert/output.txt")));
    }

    SECTION("an empty path is not denied")
    {
        CHECK_FALSE(mgr.is_denied_path_keyword(fs::path()));
    }
}

namespace {

// Converts paths as UTF-8 for the test's lifetime, as the app does from startup
// (boost::nowide::nowide_filesystem() in OrcaSlicer.cpp).
struct Utf8PathConversion
{
    std::locale previous = boost::nowide::nowide_filesystem();
    ~Utf8PathConversion() { fs::path::imbue(previous); }
};

// Switches the C library's character classification to a single-byte code page, as the app's UI
// language does on Windows (Russian_Russia.1251), when the system has one.
struct SingleByteCtypeLocale
{
    std::string previous;
    bool        applied = false;

    SingleByteCtypeLocale()
    {
        if (const char* current = std::setlocale(LC_CTYPE, nullptr))
            previous = current;
        applied = std::setlocale(LC_CTYPE, ".1251") != nullptr;
    }
    ~SingleByteCtypeLocale()
    {
        if (applied)
            std::setlocale(LC_CTYPE, previous.c_str());
    }
};

} // namespace

TEST_CASE("Plugin audit keyword check keeps non-Latin path names intact", "[audit]")
{
    seed_denied_keywords();
    const PluginAuditManager& mgr = PluginAuditManager::instance();

    // Lowering these names byte by byte in a single-byte C locale broke their UTF-8, and turning
    // the result back into a path threw out of the audit hook: the plugin call never returned.
    const Utf8PathConversion    utf8_paths;
    const SingleByteCtypeLocale ctype;
    INFO("single-byte C locale in use: " << (ctype.applied ? "yes" : "no"));

    const std::string folder   = "\xD0\x9D\xD0\xBE\xD0\xB2\xD0\xB0\xD1\x8F \xD0\xBF\xD0\xB0\xD0\xBF\xD0\xBA\xD0\xB0"; // "Новая папка"
    const std::string part     = "\xD0\xB4\xD0\xB5\xD1\x82\xD0\xB0\xD0\xBB\xD1\x8C.stl";                          // "деталь.stl"
    const std::string settings = "\xD0\xBD\xD0\xB0\xD1\x81\xD1\x82\xD1\x80\xD0\xBE\xD0\xB9\xD0\xBA\xD0\xB8.INI";      // "настройки.INI"

    SECTION("a path with non-Latin names is checked without an error and allowed")
    {
        bool denied = true;
        CHECK_NOTHROW(denied = mgr.is_denied_path_keyword(fs::path("/models/" + folder + "/" + part)));
        CHECK_FALSE(denied);
    }

    SECTION("keywords and config files next to non-Latin names are still denied, in any case")
    {
        CHECK(mgr.is_denied_path_keyword(fs::path("/models/" + folder + "/CONFIG/a.stl")));
        CHECK(mgr.is_denied_path_keyword(fs::path("/models/" + folder + "/" + settings)));
    }
}

TEST_CASE("Plugin audit checks denied keywords below data_dir, not in the directories above it", "[PluginAudit]")
{
    // Flatpak keeps the app's data folder under <home>/.var/app/<app-id>/config/OrcaSlicer, so
    // data_dir() itself has a "config" component. That ancestor describes the sandbox's layout,
    // not what the folder holds, and must not hard-deny every path inside it.
    ScopedDataDir data_dir_guard("plugin-audit-data-dir-keyword");
    const fs::path flatpak_data_dir = data_dir_guard.dir / "config" / "OrcaSlicer";
    set_data_dir(flatpak_data_dir.string());

    seed_denied_names();
    seed_denied_keywords();

    PluginAuditManager& mgr = PluginAuditManager::instance();
    // Reproduce install_hook()'s grant: data_dir() is a global allowed root.
    mgr.add_global_allowed_root(data_dir());

    ScopedPluginAuditContext ctx("test_plugin", "");

    SECTION("the root and the paths below it that carry no keyword are allowed")
    {
        CHECK_FALSE(mgr.is_denied_path_keyword(flatpak_data_dir / "orca_plugins" / "state.json"));
        CHECK(mgr.check_open(data_dir(), "r").allowed);
        CHECK(mgr.check_open((flatpak_data_dir / "orca_plugins" / "test_plugin" / "state.json").string(), "r").allowed);
        CHECK(mgr.check_open((flatpak_data_dir / "orca_plugins" / "test_plugin" / "state.json").string(), "w").allowed);
        CHECK(mgr.check_open((flatpak_data_dir / "system" / "profiles" / "vendor.json").string(), "r").allowed);
    }

    SECTION("a 'secrets' component below data_dir() is still denied")
    {
        AuditDecision decision = mgr.check_open((flatpak_data_dir / "secrets" / "token.json").string(), "r");
        CHECK_FALSE(decision.allowed);
        CHECK(decision.reason == "denied path keyword");
    }

    SECTION("a 'config' component below data_dir() is still denied")
    {
        AuditDecision decision = mgr.check_open((flatpak_data_dir / "config" / "settings.json").string(), "r");
        CHECK_FALSE(decision.allowed);
        CHECK(decision.reason == "denied path keyword");
    }

    SECTION("the app config below data_dir() is still denied by name")
    {
        AuditDecision decision = mgr.check_open((flatpak_data_dir / (SLIC3R_APP_KEY ".conf")).string(), "r");
        CHECK_FALSE(decision.allowed);
        CHECK(decision.reason == "denied filename");
    }

    SECTION("an unrelated resources root keeps the full-path keyword check")
    {
        ScopedResourcesDir resources_dir_guard("plugin-audit-data-dir-keyword-resources");
        mgr.add_global_allowed_root(resources_dir(), /*allow_write=*/false);

        CHECK(mgr.check_open((fs::path(resources_dir()) / "profiles" / "vendor.json").string(), "r").allowed);

        AuditDecision decision = mgr.check_open((fs::path(resources_dir()) / "cert" / "slicer_base64.cer").string(), "r");
        CHECK_FALSE(decision.allowed);
        CHECK(decision.reason == "denied path keyword");
    }
}

TEST_CASE("Plugin audit is_denied_path combines the filename and keyword registries", "[audit]")
{
    seed_denied_names();
    seed_denied_keywords();
    const PluginAuditManager& mgr = PluginAuditManager::instance();

    SECTION("a filename-registry match is denied")
    {
        CHECK(mgr.is_denied_path(fs::path(SLIC3R_APP_KEY ".conf")));
    }

    SECTION("a keyword-registry match is denied")
    {
        CHECK(mgr.is_denied_path(fs::path("/plugin/secrets/token.txt")));
    }

    SECTION("a path matching neither registry is not denied")
    {
        CHECK_FALSE(mgr.is_denied_path(fs::path("/plugin/output/model.gcode")));
    }
}

TEST_CASE("Plugin audit exempts the plugin's own obn.conf from the config keyword deny", "[audit]")
{
    ScopedDataDir data_dir_guard("plugin-audit-obn-conf");
    seed_denied_names();
    seed_denied_keywords();

    PluginAuditManager& mgr = PluginAuditManager::instance();
    mgr.add_global_allowed_root(data_dir());

    ScopedPluginAuditContext ctx("test_plugin", "");

    const fs::path obn_conf      = fs::path(data_dir()) / "obn.conf";
    const fs::path obn_conf_bak  = fs::path(data_dir()) / "obn.conf.bak";

    SECTION("the installer can write obn.conf inside data_dir")
    {
        // Regression: the "conf" keyword denied obn.conf before the allowed-root cascade ran,
        // so every plugin install failed with "Could not update obn.conf (...)".
        CHECK(mgr.check_open(obn_conf.string(), "w").allowed);
    }

    SECTION("the installer can read obn.conf inside data_dir")
    {
        CHECK(mgr.check_open(obn_conf.string(), "r").allowed);
    }

    SECTION("is_denied_path lets obn.conf through while the raw keyword rule still fires")
    {
        CHECK_FALSE(mgr.is_denied_path(obn_conf));
        CHECK(mgr.is_denied_path_keyword(obn_conf));
    }

    SECTION("the exemption is case-insensitive on every platform")
    {
        CHECK_FALSE(mgr.is_denied_path(fs::path(data_dir()) / "OBN.CONF"));
    }

    SECTION("a companion of obn.conf keeps matching the keyword rule")
    {
        CHECK(mgr.is_denied_path(obn_conf_bak));
    }

    SECTION("another .conf file in the same directory stays denied")
    {
        CHECK(mgr.is_denied_path(fs::path(data_dir()) / "other.conf"));
    }

    SECTION("the app config in the same directory stays denied by the filename registry")
    {
        CHECK(mgr.is_denied_path(fs::path(data_dir()) / (SLIC3R_APP_KEY ".conf")));
    }
}

TEST_CASE("Plugin audit a read-only allowed root blocks writes but not reads", "[audit]")
{
    ScopedDataDir      data_dir_guard("plugin-audit-readonly");
    ScopedResourcesDir resources_dir_guard("plugin-audit-readonly-resources");
    seed_denied_names();
    seed_denied_keywords();

    PluginAuditManager& mgr = PluginAuditManager::instance();
    mgr.add_global_allowed_root(resources_dir(), /*allow_write=*/false);

    ScopedPluginAuditContext ctx("test_plugin", "");

    const fs::path readonly_file = fs::path(resources_dir()) / "profiles" / "vendor.json";

    SECTION("a read inside the read-only root is allowed")
    {
        CHECK(mgr.check_open(readonly_file.string(), "r").allowed);
    }

    SECTION("a write inside the read-only root is blocked")
    {
        AuditDecision decision = mgr.check_open(readonly_file.string(), "w");
        CHECK_FALSE(decision.allowed);
        CHECK(decision.reason == "outside allowed root");
    }

    SECTION("a create inside the read-only root is blocked")
    {
        AuditDecision decision = mgr.check_path_access(readonly_file, /*is_write=*/true);
        CHECK_FALSE(decision.allowed);
    }

    SECTION("the bundled cert underneath the read-only root is denied even for reads")
    {
        const fs::path cert = fs::path(resources_dir()) / "cert" / "slicer_base64.cer";
        AuditDecision  decision = mgr.check_open(cert.string(), "r");
        CHECK_FALSE(decision.allowed);
        CHECK(decision.reason == "denied path keyword");
    }
}

TEST_CASE("Plugin audit network permissions match by scheme and host", "[audit]")
{
    SECTION("a URL is reduced to its scheme and host")
    {
        CHECK(PluginAuditManager::normalize_network_url("https://API.Example.com:8443/v1/items?page=2#top") ==
              "https://api.example.com");
        CHECK(PluginAuditManager::normalize_network_url("http://example.com") == "http://example.com");
        CHECK(PluginAuditManager::normalize_network_url("https://[2001:db8::1]:443/path") == "https://[2001:db8::1]");
    }

    SECTION("non-http schemes and targets without a scheme are not network URLs")
    {
        CHECK(PluginAuditManager::normalize_network_url("ftp://example.com").empty());
        CHECK(PluginAuditManager::normalize_network_url("api.example.com").empty());
        CHECK(PluginAuditManager::normalize_network_url("https://").empty());
    }

    SECTION("a URL target matches the same scheme and host regardless of path and port")
    {
        const std::vector<std::string> granted{"https://api.example.com"};
        CHECK(PluginAuditManager::network_permission_matches(granted, "https://api.example.com/v1/items?page=2"));
        CHECK(PluginAuditManager::network_permission_matches(granted, "https://api.example.com:8443/upload"));
    }

    SECTION("canonical host grants are broad but persisted full-URL grants stay exact")
    {
        const std::vector<std::string> declared_host{"https://api.example.com"};
        CHECK(PluginAuditManager::network_permission_matches(declared_host, "https://api.example.com/v1"));

        const std::vector<std::string> saved_url{"https://api.example.com/v1"};
        CHECK(PluginAuditManager::network_permission_matches(saved_url, "https://api.example.com/v1"));
        CHECK_FALSE(PluginAuditManager::network_permission_matches(saved_url, "https://api.example.com/v2"));
    }

    SECTION("a URL target does not match a different host or a different scheme")
    {
        const std::vector<std::string> granted{"https://api.example.com"};
        CHECK_FALSE(PluginAuditManager::network_permission_matches(granted, "https://evil.example.com/v1"));
        CHECK_FALSE(PluginAuditManager::network_permission_matches(granted, "http://api.example.com/v1"));
    }

    SECTION("a bare host target matches the host of a grant")
    {
        const std::vector<std::string> granted{"https://api.example.com"};
        CHECK(PluginAuditManager::network_permission_matches(granted, "api.example.com"));
        CHECK(PluginAuditManager::network_permission_matches(granted, "api.example.com:443"));
        CHECK_FALSE(PluginAuditManager::network_permission_matches(granted, "other.example.com"));
    }

    SECTION("a saved full URL does not authorize raw socket access to its host")
    {
        const std::vector<std::string> saved_url{"https://api.example.com/v1"};
        CHECK(PluginAuditManager::network_permission_matches(saved_url, "https://api.example.com/v1"));
        CHECK_FALSE(PluginAuditManager::network_permission_matches(saved_url, "api.example.com"));
        CHECK_FALSE(PluginAuditManager::network_permission_matches(saved_url, "api.example.com:443"));
        CHECK_FALSE(PluginAuditManager::network_permission_matches(saved_url, "('api.example.com', 443)"));

        const std::vector<std::string> saved_ipv6{"https://[2001:db8::1]/v1"};
        CHECK(PluginAuditManager::network_permission_matches(saved_ipv6, "https://[2001:db8::1]/v1"));
        CHECK_FALSE(PluginAuditManager::network_permission_matches(saved_ipv6, "2001:db8::1"));
        CHECK_FALSE(PluginAuditManager::network_permission_matches(saved_ipv6, "[2001:db8::1]:443"));

        // Canonical scheme://host grants still cover raw socket targets, IPv6 brackets aside.
        const std::vector<std::string> canonical{"https://api.example.com", "https://[2001:db8::1]"};
        CHECK(PluginAuditManager::network_permission_matches(canonical, "api.example.com"));
        CHECK(PluginAuditManager::network_permission_matches(canonical, "2001:db8::1"));

        // Normalization lowercases the scheme and host, so case alone keeps a grant canonical.
        const std::vector<std::string> uppercase{"HTTPS://API.Example.com", "HTTPS://[2001:DB8::1]"};
        CHECK(PluginAuditManager::network_permission_matches(uppercase, "api.example.com"));
        CHECK(PluginAuditManager::network_permission_matches(uppercase, "2001:db8::1"));
    }

    SECTION("legacy persisted address tuples keep matching their host")
    {
        const std::vector<std::string> legacy_granted{"('api.example.com', 443)"};
        CHECK(PluginAuditManager::network_permission_matches(legacy_granted, "api.example.com"));

        const std::vector<std::string> granted{"https://api.example.com"};
        CHECK(PluginAuditManager::network_permission_matches(granted, "('api.example.com', 443)"));
        CHECK_FALSE(PluginAuditManager::network_permission_matches(granted, "('93.184.216.34', 443)"));
    }

    SECTION("an IPv6 literal matches across bracketed and bare forms")
    {
        const std::vector<std::string> granted{"https://[2001:db8::1]"};
        CHECK(PluginAuditManager::network_permission_matches(granted, "2001:db8::1"));
        CHECK(PluginAuditManager::network_permission_matches(granted, "[2001:db8::1]:443"));
        CHECK_FALSE(PluginAuditManager::network_permission_matches(granted, "2001:db8::2"));
    }

    SECTION("a non-http URL only matches an exact grant")
    {
        const std::vector<std::string> granted{"ftp://example.com/file"};
        CHECK(PluginAuditManager::network_permission_matches(granted, "ftp://example.com/file"));
        CHECK_FALSE(PluginAuditManager::network_permission_matches(granted, "ftp://example.com/other"));

        const std::vector<std::string> https{"https://api.example.com"};
        CHECK_FALSE(PluginAuditManager::network_permission_matches(https, "ftp://api.example.com/file"));
    }

    SECTION("an empty grant list or an empty target never matches")
    {
        const std::vector<std::string> granted{};
        CHECK_FALSE(PluginAuditManager::network_permission_matches(granted, "https://api.example.com"));

        const std::vector<std::string> any{"https://api.example.com"};
        CHECK_FALSE(PluginAuditManager::network_permission_matches(any, ""));
    }
}

TEST_CASE("Plugin audit a scoped root can also be registered read-only", "[audit]")
{
    ScopedDataDir data_dir_guard("plugin-audit-scoped-readonly");
    seed_denied_names();
    seed_denied_keywords();

    PluginAuditManager& mgr = PluginAuditManager::instance();

    const fs::path readonly_dir = fs::path(data_dir()) / "readonly_scope";
    fs::create_directories(readonly_dir);

    ScopedPluginAuditContext ctx("test_plugin", "");
    mgr.add_scoped_allowed_root(readonly_dir, /*allow_write=*/false);

    SECTION("a read inside the scoped read-only root is allowed")
    {
        CHECK(mgr.check_open((readonly_dir / "vendor.json").string(), "r").allowed);
    }

    SECTION("a write inside the scoped read-only root is blocked")
    {
        CHECK_FALSE(mgr.check_open((readonly_dir / "vendor.json").string(), "w").allowed);
    }
}

// ---------------------------------------------------------------------------
// The audit hook, run in the embedded interpreter
// ---------------------------------------------------------------------------
//
// Each TEST_CASE runs in its own process (catch_discover_tests), so each brings the plugin system
// up with the audit hook installed, once, and shuts it down at the end. The tests loop over their
// cases instead of using SECTIONs, which would restart the interpreter for every section.

namespace {

// Discovered, never loaded: the hook only needs the plugin's root and its sidecar.
const char* const AUDIT_PLUGIN_SOURCE = R"PY(# /// script
# requires-python = ">=3.12"
#
# [tool.orcaslicer.plugin]
# name = "Audit Plugin"
# description = "Plugin audit hook fixture"
# author = "OrcaSlicer"
# version = "1.0"
# type = "script"
# ///
)PY";

// A plugin whose registration fails after the module has imported. Its queued runtime prompt is
// the one an ordinary load failure must keep actionable for approval and retry.
const char* const BROKEN_AUDIT_PLUGIN_SOURCE = R"PY(# /// script
# requires-python = ">=3.12"
#
# [tool.orcaslicer.plugin]
# name = "Broken Audit Plugin"
# description = "Plugin audit load-failure fixture"
# author = "OrcaSlicer"
# version = "1.0"
# type = "script"
# ///
import orca

@orca.plugin
class BrokenAuditPackage(orca.base):
    def register_capabilities(self):
        raise RuntimeError("registration failed")
)PY";

// A third-party library, installed outside the stdlib and outside the plugin.
const char* const LIBRARY_SOURCE = R"PY(
import socket
import urllib.request

def read_all(paths):
    for path in paths:
        with open(path) as f:
            f.read()

def connect_from_host(port):
    socket.gethostname()
    socket.create_connection(("127.0.0.1", port)).close()

def fetch(url):
    with urllib.request.urlopen(url) as response:
        return response.read()

def fetch_each(urls):
    for url in urls:
        yield fetch(url)

class ReadingHandler(urllib.request.BaseHandler):
    """Reads a file while each response is processed."""
    def __init__(self, path):
        self.path = path
        self.outcome = None

    def http_response(self, request, response):
        try:
            open(self.path).close()
            self.outcome = "ok"
        except PermissionError:
            self.outcome = "denied"
        return response
)PY";

// Test-side Python, run outside any plugin context: files outside every allowed root, a local
// HTTP server, the library, and every module the tests use imported up front, so that no import
// happens inside a plugin context. Code from exec() runs as "<string>", which is not stdlib, so
// attempt() and fetch() stand in for plugin code.
const char* const AUDIT_SETUP = R"PY(
import http.client, http.server, os, pathlib, shutil, socket, subprocess, sys, tempfile, threading
import urllib.request, zipfile
if os.name != "nt":
    import pty
else:
    import _winapi

APPROVED = os.path.join(OUTSIDE, "approved.txt")
PRIVATE = os.path.join(OUTSIDE, "private.txt")
MISSING = os.path.join(OUTSIDE, "missing")
TREE = os.path.join(OUTSIDE, "tree")
TRUE_COMMAND = ["cmd", "/c", "exit 0"] if os.name == "nt" else ["true"]

# The shells os.system may run a command in. On Windows the runtime uses COMSPEC or cmd.exe,
# and falls back to cmd.exe when COMSPEC names a file it cannot open, so both are targets. A
# bare cmd.exe COMSPEC is the fallback itself.
if os.name != "nt":
    SYSTEM_SHELLS = ["/bin/sh"]
else:
    COMSPEC = os.environ.get("COMSPEC") or ""
    SYSTEM_SHELLS = (["cmd.exe"] if not COMSPEC or
                     ("/" not in COMSPEC and "\\" not in COMSPEC and COMSPEC.lower() == "cmd.exe")
                     else [COMSPEC, "cmd.exe"])
for path in (APPROVED, PRIVATE):
    with open(path, "w") as f:
        f.write("x")
for sub in ("one", "two"):
    os.makedirs(os.path.join(TREE, sub))
    with open(os.path.join(TREE, sub, "f.txt"), "w") as f:
        f.write("x")

LIBRARY = os.path.join(OUTSIDE, "library")
os.makedirs(LIBRARY)
with open(os.path.join(LIBRARY, "netlib.py"), "w") as f:
    f.write(LIBRARY_SOURCE)
sys.path.insert(0, LIBRARY)
import netlib

class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/redirect":
            self.send_response(302)
            self.send_header("Location", URL_B)
            body = b""
        else:
            self.send_response(200)
            body = b"ok"
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass

server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
server_thread = threading.Thread(target=server.serve_forever, daemon=True)
server_thread.start()
PORT = server.server_address[1]
BASE = "http://127.0.0.1:%d" % PORT
URL_A, URL_B, URL_REDIRECT = BASE + "/a", BASE + "/b", BASE + "/redirect"
urllib.request.install_opener(urllib.request.build_opener(urllib.request.ProxyHandler({})))

def fetch(url):
    with urllib.request.urlopen(url) as response:
        return response.read()

fetch(URL_A)  # urllib imports some modules on its first request

def attempt(action):
    try:
        action()
        return "ok"
    except PermissionError:
        return "denied"

def start_and_join_thread():
    worker = threading.Thread(target=lambda: None)
    worker.start()
    worker.join()

def spawn_checked(argv):
    parent = os.getpid()
    try:
        return pty.spawn(argv)
    finally:
        if os.getpid() != parent:
            os._exit(1)  # a child whose exec was refused must not go on running the tests

finalizer_outcomes = []

class TrackedRequest(urllib.request.Request):
    def __del__(self):
        finalizer_outcomes.append(attempt(lambda: open(PRIVATE).close()))
)PY";

struct Prompt
{
    std::string                                            plugin;
    std::string                                            event;
    std::vector<std::string>                               targets;
    std::vector<PluginAuditManager::PermissionPromptEntry> entries;
};

// One line per prompt, "event: target ...", so a failed check shows every prompt.
std::string describe(const std::vector<Prompt>& prompts)
{
    std::string text;
    for (const Prompt& prompt : prompts) {
        text += prompt.event + ":";
        for (const std::string& target : prompt.targets)
            text += " " + target;
        text += "\n";
    }
    return text;
}

// The line describe() prints for an os.system prompt, whose target is every possible shell.
std::string describe_system_prompt(const std::vector<std::string>& shells)
{
    std::string text = "os.system:";
    for (const std::string& shell : shells)
        text += " " + shell;
    return text + "\n";
}

// The process grants a successful os.system approval adds to the ones already saved.
std::vector<std::string> with_shell_grants(std::vector<std::string> grants,
                                           const std::vector<std::string>& shells)
{
    grants.insert(grants.end(), shells.begin(), shells.end());
    return grants;
}

// The plugin system with the audit hook installed, and the permission dialog replaced by a
// recorder that answers with `approve`. Declare it first in a test, so it is destroyed last.
//
// Only the manager is torn down here; the interpreter is finalized once per test process by
// PythonInterpreterRunEnd in slic3rutils_tests_main.cpp.
struct AuditHarness
{
    // Before initialize(): the interpreter creates {data_dir}/python/packages and {data_dir}/log.
    // data_dir() is a global allowed root from then on -- the first test to initialize Python in
    // this process (under ctest, this one) -- so the test files live in `outside`.
    ScopedDataDir                      data{"plugin-audit-hook"};
    bool                               initialized = PluginManager::instance().initialize();
    ScopedTemporaryDir                 outside{"orca-plugin-audit-outside"};
    std::vector<Prompt>                prompts;
    std::function<bool(const Prompt&)> approve = [](const Prompt&) { return false; };
    std::function<bool(const Prompt&)> approve_recursive_delete = [](const Prompt&) { return false; };

    AuditHarness()
    {
        PluginAuditManager& audit = PluginAuditManager::instance();
        audit.clear_current_plugin();
        audit.clear_current_capability();
        audit.clear_audit_denial();
        audit.clear_last_violation();
        PluginAuditManager::clear_recursive_delete_approvals_for_testing();
        PluginAuditManager::set_recursive_delete_approval_lifetime_for_testing(std::chrono::minutes(1));
        PluginAuditManager::set_recursive_delete_prompt_background_for_testing(false);
        PluginAuditManager::set_recursive_delete_prompt_deferred_for_testing(false);
        PluginAuditManager::set_permission_prompt_for_testing(
            [this](const std::string& event, const std::vector<std::string>& targets,
                   const std::vector<PluginAuditManager::PermissionPromptEntry>& entries) {
                prompts.push_back({PluginAuditManager::instance().current_plugin(), event, targets, entries});
                return approve(prompts.back());
            });
        PluginAuditManager::set_recursive_delete_prompt_for_testing(
            [this](const std::string&, const std::string& target) {
                prompts.push_back({PluginAuditManager::instance().current_plugin(), "shutil.rmtree", {target},
                                   {{target, ""}}});
                return approve_recursive_delete(prompts.back());
            });
    }

    ~AuditHarness()
    {
        PluginAuditManager::set_permission_prompt_for_testing(nullptr);
        PluginAuditManager::set_recursive_delete_prompt_for_testing(nullptr);
        PluginAuditManager::set_recursive_delete_prompt_background_for_testing(false);
        PluginAuditManager::set_recursive_delete_prompt_deferred_for_testing(false);
        PluginAuditManager::clear_recursive_delete_approvals_for_testing();
        PluginAuditManager::set_recursive_delete_approval_lifetime_for_testing(std::chrono::minutes(1));
        PluginManager::instance().shutdown();
    }

    // Call before taking the GIL: discovery is not Python work.
    void add_plugin(const std::string& key, const char* source = AUDIT_PLUGIN_SOURCE)
    {
        const fs::path root = data.plugins_dir() / key;
        fs::create_directories(root);
        std::ofstream((root / (key + ".py")).string(), std::ios::binary) << source;
        save_permissions(key, {});
        PluginManager::instance().discover_plugins(/*async=*/false, /*clear=*/true);
    }

    // Grants as if saved in an earlier session.
    void save_permissions(const std::string& key, const PluginPermissions& permissions)
    {
        PluginInstallState state;
        state.installed_from    = "local";
        state.installed_version = "1.0";
        state.plugin_name       = key;
        state.permissions       = permissions;
        REQUIRE(write_install_state(data.plugins_dir() / key, state));
    }

    PluginPermissions saved_permissions(const std::string& key) const
    {
        PluginInstallState state;
        read_install_state(data.plugins_dir() / key, state);
        return state.permissions;
    }
};

// The test-side Python globals. Construct and destroy it with the GIL held; it stops the HTTP
// server before the test case ends, so nothing keeps serving once the test's temp dirs are gone.
struct PythonSide
{
    py::dict globals;

    explicit PythonSide(const AuditHarness& harness)
    {
        globals["OUTSIDE"]        = harness.outside.path().string();
        globals["LIBRARY_SOURCE"] = LIBRARY_SOURCE;
        exec(AUDIT_SETUP);
    }

    // A Python error fails the test here, as text: an error_already_set left to propagate would
    // outlive the test case and keep a traceback with references into the interpreter.
    void exec(const char* code)
    {
        std::string error;
        try {
            py::exec(code, globals);
        } catch (py::error_already_set& raised) {
            error = raised.what();
        }
        if (!error.empty())
            FAIL(error);
    }

    ~PythonSide()
    {
        try {
            // Stop the server thread while Python is still up: the interpreter now outlives the
            // test case, so a daemon thread must not keep serving (and touching test temp dirs).
            globals["server"].attr("shutdown")();
            globals["server"].attr("server_close")();
            globals["server_thread"].attr("join")();
        } catch (...) {
        }
    }

    std::string              str(const char* name) const { return py::str(globals[name]).cast<std::string>(); }
    std::string              eval(const char* expression) const { return py::str(py::eval(expression, globals)).cast<std::string>(); }
    std::vector<std::string> strings(const char* name) const { return globals[name].cast<std::vector<std::string>>(); }

    // Runs `code` as `plugin_key`, as a trampoline call does: inside the caller's GIL, in an
    // audit scope of its own.
    void run_as(const std::string& plugin_key, const char* code)
    {
        ScopedPluginAuditContext audit(plugin_key);
        exec(code);
    }
};

PluginPermissions http_grants(std::vector<std::string> urls)
{
    PluginPermissions permissions;
    permissions.network_http = std::move(urls);
    return permissions;
}

const char* const PLUGIN = "AuditPlugin";

} // namespace

TEST_CASE("A queued runtime prompt survives its plugin's failed load until unload", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedRuntimePromptQueue runtime_queue;
    harness.add_plugin("Broken_Plugin", BROKEN_AUDIT_PLUGIN_SOURCE);

    // A prompt for another plugin is on screen when the failing plugin raises its event.
    CHECK(runtime_queue.queue.admit(prompt_request("active", AuditEventCategory::FsRead, {"/active"}),
                                    /*may_show_now=*/true) == AuditPromptQueue::Admission::ShowNow);
    CHECK(runtime_queue.queue.admit(prompt_request("Broken_Plugin", AuditEventCategory::FsRead, {"/queued"}),
                                    /*may_show_now=*/false) == AuditPromptQueue::Admission::Queued);
    CHECK(runtime_queue.queue.pending_count() == 1);

    PluginManager& manager = PluginManager::instance();
    manager.load_plugin("Broken_Plugin", /*skip_deps=*/true);
    std::string error;
    CHECK_FALSE(manager.wait_for_plugin_load("Broken_Plugin", std::chrono::seconds(120), error));
    INFO("load error: " << error);
    CHECK(manager.is_plugin_load_failed("Broken_Plugin"));
    CHECK(runtime_queue.queue.pending_count() == 1);

    // The active prompt closes; the queued runtime prompt is next and still actionable even
    // though its load has already failed and released the slot.
    runtime_queue.queue.complete();
    std::optional<AuditPromptRequest> next = runtime_queue.queue.take_next();
    REQUIRE(next.has_value());
    CHECK(next->plugin_key == "Broken_Plugin");
    CHECK(prompt_plugin_is_available(prompt_plugin_state(next->plugin_key), next->kind));
    runtime_queue.queue.complete();

    // Unloading the plugin runs the production discard against this same queue.
    CHECK(runtime_queue.queue.admit(prompt_request("Broken_Plugin", AuditEventCategory::FsRead, {"/queued"}),
                                    /*may_show_now=*/false) == AuditPromptQueue::Admission::Queued);
    REQUIRE(manager.unload_plugin("Broken_Plugin"));
    CHECK(runtime_queue.queue.pending_count() == 0);
    CHECK(runtime_queue.queue.idle());
    CHECK_FALSE(prompt_plugin_is_available(prompt_plugin_state("Broken_Plugin"), AuditPromptKind::RuntimeEvent));
}

TEST_CASE("A saved request grant covers the request's own connection in a later session", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);
    harness.save_permissions(PLUGIN, http_grants({python.str("URL_A")}));

    python.run_as(PLUGIN, "outcome = attempt(lambda: fetch(URL_A))");

    CHECK(python.str("outcome") == "ok");
    CHECK(describe(harness.prompts).empty());
}

TEST_CASE("A permission prompt that throws denies the event", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);
    harness.approve = [](const Prompt&) -> bool { throw std::runtime_error("prompt failed"); };

    python.run_as(PLUGIN, "outcome = attempt(lambda: open(PRIVATE).close())");

    CHECK(python.str("outcome") == "denied");
    CHECK(describe(harness.prompts) == "open: " + python.str("PRIVATE") + "\n");
}

TEST_CASE("A saved request grant does not cover another URL or a redirect", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);
    harness.save_permissions(PLUGIN, http_grants({python.str("URL_A"), python.str("URL_REDIRECT")}));

    python.run_as(PLUGIN, R"PY(
outcomes = [attempt(lambda: fetch(url)) for url in (URL_A, URL_B, URL_REDIRECT)]
outcomes += [attempt(lambda: netlib.fetch(url)) for url in (URL_A, URL_B)]
outcomes.append(attempt(lambda: list(netlib.fetch_each([URL_A, URL_B]))))
)PY");

    // The redirect target is asked for inside the approved redirecting request, and a library's
    // wrapper or generator asks for the second URL as plain code does.
    CHECK(python.strings("outcomes") == std::vector<std::string>{"ok", "denied", "denied", "ok", "denied", "denied"});
    const std::string url_b = "urllib.Request: " + python.str("URL_B") + "\n";
    CHECK(describe(harness.prompts) == url_b + url_b + url_b + url_b);
}

TEST_CASE("Code an approved operation calls back into is judged on its own", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);
    python.exec(R"PY(
CALLBACK_COPY = os.path.join(OUTSIDE, "callback-copy")
handler = netlib.ReadingHandler(PRIVATE)
handler_opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), handler)
walk_outcomes = []
copy_outcomes = []

def on_walk_error(error):
    walk_outcomes.append(attempt(lambda: open(PRIVATE).close()))

def copy_file(source, destination):
    copy_outcomes.append(attempt(lambda: open(PRIVATE).close()))
)PY");
    PluginPermissions permissions = http_grants({python.str("URL_A")});
    permissions.fs_read           = {python.str("APPROVED"), python.str("MISSING")};
    permissions.fs_readwrite      = {python.str("TREE"), python.str("CALLBACK_COPY")};
    harness.save_permissions(PLUGIN, permissions);

    // A library handler that urllib calls while the approved request runs, the plugin's own
    // os.walk error callback and copytree copy function (called once per file), and a library
    // loop over a saved file and an unsaved one.
    python.run_as(PLUGIN, R"PY(
request_outcome = attempt(lambda: handler_opener.open(URL_A).read())
for _ in os.walk(MISSING, onerror=on_walk_error):
    pass
shutil.copytree(TREE, CALLBACK_COPY, copy_function=copy_file)
loop_outcome = attempt(lambda: netlib.read_all([APPROVED, PRIVATE]))
)PY");

    CHECK(python.str("request_outcome") == "ok");
    CHECK(python.eval("handler.outcome") == "denied");
    CHECK(python.strings("walk_outcomes") == std::vector<std::string>{"denied"});
    CHECK(python.strings("copy_outcomes") == std::vector<std::string>{"denied", "denied"});
    CHECK(python.str("loop_outcome") == "denied");
    const std::string private_file = "open: " + python.str("PRIVATE") + "\n";
    CHECK(describe(harness.prompts) == private_file + private_file + private_file + private_file + private_file);
}

TEST_CASE("A saved directory walk covers each directory it lists", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);
    PluginPermissions permissions;
    permissions.fs_read = {python.str("TREE"), python.str("APPROVED")};
    harness.save_permissions(PLUGIN, permissions);

    // The walk lists the tree's two subdirectories on later resumes, after the plugin's own
    // audited reads in between. Abandoned walks leave frames whose generator is gone, which the
    // next audited event lets go.
    python.run_as(PLUGIN, R"PY(
walked = []
for root, dirs, files in os.walk(TREE):
    walked.append(root)
    open(APPROVED).close()
globbed = list(pathlib.Path(TREE).glob("**/*"))

abandoned = os.walk(TREE)
next(abandoned)
del abandoned
open(APPROVED).close()
for _ in pathlib.Path(TREE).glob("*"):
    break
open(APPROVED).close()
)PY");

    CHECK(py::len(python.globals["walked"]) == 3);
    CHECK(py::len(python.globals["globbed"]) == 4);
    CHECK(describe(harness.prompts).empty());
}

TEST_CASE("A freshly approved operation covers its nested events", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    struct Case
    {
        const char* prompts;
        const char* code;
    };
    // Each case starts with no saved grants, so its root event is asked once; a freshly approved
    // create or delete saves the parent folder, so the stored grants are reset between cases. A
    // process grant names the executable, so pty.spawn's own approval covers the exec in the
    // forked child. Only the expected prompts are approved, so a nested event left uncovered
    // fails the operation.
    std::vector<Case> cases = {
        {"shutil.copytree", "shutil.copytree(TREE, os.path.join(OUTSIDE, 'copy'))"},
        {"shutil.copyfile", "shutil.copyfile(APPROVED, os.path.join(OUTSIDE, 'copied.txt'))"},
        {"shutil.move", "shutil.move(os.path.join(OUTSIDE, 'copied.txt'), os.path.join(OUTSIDE, 'moved.txt'))"},
        {"shutil.copystat", "shutil.copystat(os.path.join(TREE, 'one', 'f.txt'), PRIVATE)"},
        {"shutil.make_archive", "shutil.make_archive(os.path.join(OUTSIDE, 'archive'), 'zip', root_dir=TREE)"},
        {"shutil.unpack_archive",
         "shutil.unpack_archive(os.path.join(OUTSIDE, 'archive.zip'), os.path.join(OUTSIDE, 'unpacked'))"},
        {"pathlib.Path.glob", "list(pathlib.Path(TREE).glob('**/*'))"},
        {"subprocess.Popen", "subprocess.run(TRUE_COMMAND, check=True)"},
        {"tempfile.mkdtemp", "tempfile.mkdtemp(dir=OUTSIDE)"},
        {"tempfile.mkstemp", "os.close(tempfile.mkstemp(dir=OUTSIDE)[0])"},
        // The host grant from connect() also authorizes targetless send() events.
        {"http.client.connect",
         "connection = http.client.HTTPConnection('127.0.0.1', PORT)\n"
         "connection.request('GET', '/a')\n"
         "connection.getresponse().read()\n"
         "connection.close()"},
    };
#ifndef _WIN32
    cases.push_back({"os.fwalk", "list(os.fwalk(os.path.join(OUTSIDE, 'unpacked')))"});
    // The forked child's exec is covered by the spawn too: had it prompted, the child's own
    // denial would end it with status 1.
    cases.push_back({"pty.spawn", "spawn_status = spawn_checked(TRUE_COMMAND + ['spawned'])"});
#endif

    for (const Case& operation : cases) {
        INFO(operation.code);
        // Dropping the grants from the previous case keeps its persisted create/delete parent
        // folder from covering this one.
        harness.save_permissions(PLUGIN, {});
        const std::string expected = std::string(operation.prompts) + "\n";
        harness.approve = [&expected](const Prompt& prompt) {
            return expected.find(prompt.event + "\n") != std::string::npos;
        };
        harness.prompts.clear();
        python.run_as(PLUGIN, operation.code);

        std::string asked;
        for (const Prompt& prompt : harness.prompts)
            asked += (asked.empty() ? "" : "\n") + prompt.event;
        CHECK(asked == operation.prompts);
    }
#ifndef _WIN32
    CHECK(python.eval("spawn_status") == "0");
#endif
}

TEST_CASE("An event with no target is granted by name and remembered", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    harness.approve = [](const Prompt& prompt) { return prompt.event != "socket.sendto"; };
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    python.run_as(PLUGIN, R"PY(
socket.socket().close()
udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sendto_outcome = attempt(lambda: udp.sendto(b"x", ("127.0.0.1", 9)))
udp.close()
)PY");

    // The socket is asked for once, without a target to show; the datagram's address is a target.
    CHECK(python.str("sendto_outcome") == "denied");
    CHECK(describe(harness.prompts) == "socket.__new__:\nsocket.sendto: ('127.0.0.1', 9)\n");
    CHECK(harness.saved_permissions(PLUGIN).events == std::vector<std::string>{"Socket:socket.__new__"});
    CHECK(harness.saved_permissions(PLUGIN).network_socket.empty());

    harness.prompts.clear();
    python.run_as(PLUGIN, "socket.socket().close()");
    CHECK(describe(harness.prompts).empty());

    // A grant for an event with no target covers nothing else: a library's approved
    // gethostname() does not carry the connection it makes next.
    harness.approve = [](const Prompt& prompt) { return prompt.event == "socket.gethostname"; };
    python.run_as(PLUGIN, "connect_outcome = attempt(lambda: netlib.connect_from_host(PORT))");
    CHECK(python.str("connect_outcome") == "denied");
    CHECK(describe(harness.prompts) == "socket.gethostname:\nsocket.getaddrinfo: 127.0.0.1\n");
    harness.prompts.clear();
    harness.approve = [](const Prompt&) { return true; };

#ifndef _WIN32
    // sendmsg() without an address names no target either.
    python.run_as(PLUGIN, R"PY(
left, right = socket.socketpair()
left.sendmsg([b"x"])
left.close()
right.close()
)PY");
    CHECK(describe(harness.prompts) == "socket.sendmsg:\n");
    CHECK(harness.saved_permissions(PLUGIN).events ==
          std::vector<std::string>{"Socket:socket.__new__", "Socket:socket.gethostname", "Socket:socket.sendmsg"});
#endif
}

TEST_CASE("A saved network grant covers a network event that names no target", "[Audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    // The connection is opened outside the plugin, so only the send itself is judged: it names
    // no host, just the connection it writes to.
    python.exec(R"PY(
connection = http.client.HTTPConnection("127.0.0.1", PORT)
connection.connect()
)PY");
    harness.save_permissions(PLUGIN, http_grants({"http://127.0.0.1"}));

    python.run_as(PLUGIN, R"PY(
send_outcome = attempt(lambda: connection.send(b"GET /"))
)PY");

    // The host grant already covers the connection's send: no prompt, and no event grant.
    CHECK(python.str("send_outcome") == "ok");
    CHECK(describe(harness.prompts).empty());
    CHECK(harness.saved_permissions(PLUGIN).events.empty());

    // With no network grant there is nothing covering the send, so it is asked for by name.
    harness.save_permissions(PLUGIN, http_grants({}));
    python.run_as(PLUGIN, R"PY(
denied_outcome = attempt(lambda: connection.send(b"GET /"))
)PY");
    CHECK(python.str("denied_outcome") == "denied");
    CHECK(describe(harness.prompts) == "http.client.send:\n");
    CHECK(harness.saved_permissions(PLUGIN).events.empty());
    CHECK(harness.saved_permissions(PLUGIN).network_http.empty());

    python.exec("connection.close()");
}

TEST_CASE("An event grant does not stand in for another access or a target of the same name", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    harness.approve = [asked = 0](const Prompt&) mutable { return asked++ == 0; };
    py::gil_scoped_acquire gil;
    PythonSide python(harness);
    python.exec(R"PY(
READ_FD = os.open(APPROVED, os.O_RDONLY)
WRITE_FD = os.open(os.path.join(OUTSIDE, "scratch.txt"), os.O_WRONLY | os.O_CREAT)
)PY");

    // A file descriptor has no path to show or save: a granted read of one is not a write.
    python.run_as(PLUGIN, R"PY(
fd_outcomes = [attempt(lambda: open(READ_FD, "r", closefd=False).close()),
               attempt(lambda: open(WRITE_FD, "w", closefd=False).close())]
)PY");
    CHECK(python.strings("fd_outcomes") == std::vector<std::string>{"ok", "denied"});
    CHECK(describe(harness.prompts) == "open:\nopen:\n");
    CHECK(harness.saved_permissions(PLUGIN).events == std::vector<std::string>{"FsRead:open"});

    // A saved os.fork event grant is not the shell os.system runs.
    PluginPermissions permissions;
    permissions.events = {"FsRead:open", "ProcessCreate:os.fork"};
    harness.save_permissions(PLUGIN, permissions);
    harness.prompts.clear();
    python.run_as(PLUGIN, R"PY(
reread_outcome = attempt(lambda: open(READ_FD, "r", closefd=False).close())
system_outcome = attempt(lambda: os.system("os.fork"))
os.close(READ_FD)
os.close(WRITE_FD)
)PY");
    CHECK(python.str("reread_outcome") == "ok");
    CHECK(python.str("system_outcome") == "denied");
    REQUIRE(harness.prompts.size() == 1);
    CHECK(harness.prompts.front().event == "os.system");
    CHECK(harness.saved_permissions(PLUGIN).process.empty());
}

TEST_CASE("A process grant names the executable, or the exact Windows command line", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);
    const std::vector<std::string> shells = python.strings("SYSTEM_SHELLS");
#ifdef _WIN32
    // subprocess.run() joins argv into one command line before the audit event. Its executable
    // "cmd" is unquoted and followed by arguments, so the documented grant keeps the whole exact
    // line instead of the first token.
    const std::string popen_target = R"(CommandLine:cmd /c "exit 0")";
#else
    // POSIX keeps the executable in its own argv element, so only the executable is granted.
    const std::string popen_target = python.eval("TRUE_COMMAND[0]");
#endif

    // Refused, so nothing runs: the prompts show the target each event reports. A direct process
    // event names the executable, except a Windows command line whose unquoted executable is
    // followed by arguments, which is the exact command line. os.system names every shell that may
    // parse its command string.
    python.run_as(PLUGIN, R"PY(
popen_outcome = attempt(lambda: subprocess.run(TRUE_COMMAND, check=True))
system_outcome = attempt(lambda: os.system(" ".join(TRUE_COMMAND) + " ignored"))
)PY");
    CHECK(python.str("popen_outcome") == "denied");
    CHECK(python.str("system_outcome") == "denied");
    CHECK(describe(harness.prompts) ==
          "subprocess.Popen: " + popen_target + "\n" + describe_system_prompt(shells));

#ifndef _WIN32
    harness.prompts.clear();
    python.run_as(PLUGIN, "spawn_outcome = attempt(lambda: spawn_checked(TRUE_COMMAND + ['spawned']))");
    CHECK(python.str("spawn_outcome") == "denied");
    CHECK(describe(harness.prompts) == "pty.spawn: " + popen_target + "\n");
#endif

    // Approving the direct process event grants its target alone: on Windows the exact command
    // line, elsewhere the executable. The approval also opens an operation frame that covers the
    // runtime's nested CreateProcess event, so the run asks once. Neither target is a shell grant,
    // so a later os.system is asked for on its own.
    harness.approve = [](const Prompt&) { return true; };
    harness.prompts.clear();
    python.run_as(PLUGIN, "popen_outcome = attempt(lambda: subprocess.run(TRUE_COMMAND, check=True))");
    CHECK(python.str("popen_outcome") == "ok");
    REQUIRE(harness.prompts.size() == 1);
    CHECK(harness.prompts.front().targets == std::vector<std::string>{popen_target});
    CHECK(harness.saved_permissions(PLUGIN).process == std::vector<std::string>{popen_target});

    harness.prompts.clear();
    python.run_as(PLUGIN, R"PY(
system_outcome = attempt(lambda: os.system(" ".join(TRUE_COMMAND) + " ignored"))
)PY");
    CHECK(python.str("system_outcome") == "ok");
    REQUIRE(harness.prompts.size() == 1);
    CHECK(harness.prompts.front().targets == shells);
    CHECK(harness.saved_permissions(PLUGIN).process ==
          with_shell_grants({popen_target}, shells));

    // The shell grant covers a different command string, arguments included.
    harness.prompts.clear();
    python.run_as(PLUGIN, R"PY(
repeat_outcome = attempt(lambda: os.system(" ".join(TRUE_COMMAND) + " another argument"))
)PY");
    CHECK(python.str("repeat_outcome") == "ok");
    CHECK(describe(harness.prompts).empty());
}

TEST_CASE("A subprocess grant names the executable from a Windows command line", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    // The event Windows subprocess.Popen raises: the executable is unresolved unless the caller
    // passed one, and the arguments are a single command line. Raising it directly checks the
    // target the hook reads without creating a process.
    python.run_as(PLUGIN, R"PY(
def raise_popen(executable, command_line):
    try:
        sys.audit("subprocess.Popen", executable, command_line, None, None)
        return "ok"
    except PermissionError:
        return "denied"

quoted_outcome = raise_popen(None, r'"C:\Program Files\Tool\tool.exe" --flag')
explicit_outcome = raise_popen(r"D:\other\tool.exe", r'"C:\Program Files\Tool\tool.exe" --flag')
unquoted_outcome = raise_popen(None, r"plain-tool.exe --flag")
)PY");
    CHECK(python.str("quoted_outcome") == "denied");
    CHECK(python.str("explicit_outcome") == "denied");
    CHECK(python.str("unquoted_outcome") == "denied");
    CHECK(describe(harness.prompts) ==
          "subprocess.Popen: C:\\Program Files\\Tool\\tool.exe\n"
          "subprocess.Popen: D:\\other\\tool.exe\n"
          "subprocess.Popen: CommandLine:plain-tool.exe --flag\n");
}

TEST_CASE("An unquoted Windows command line is granted as one exact command", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    // An existing executable grant for the first token must not authorize the longer executable
    // that CreateProcess may resolve from an unquoted command line.
    PluginPermissions legacy;
    legacy.process = {R"(C:\Program)"};
    harness.save_permissions(PLUGIN, legacy);

    python.run_as(PLUGIN, R"PY(
def raise_popen(executable, command_line):
    try:
        sys.audit("subprocess.Popen", executable, command_line, None, None)
        return "ok"
    except PermissionError:
        return "denied"

tool_a_one = r"C:\Program Files\ToolA\a.exe --one"
)PY");

    // The legacy C:\Program grant is the first token of ToolA, but the command line is asked for
    // as one exact command instead.
    python.run_as(PLUGIN, R"PY(
denied_outcome = raise_popen(None, tool_a_one)
)PY");
    CHECK(python.str("denied_outcome") == "denied");
    CHECK(describe(harness.prompts) ==
          "subprocess.Popen: CommandLine:C:\\Program Files\\ToolA\\a.exe --one\n");
    CHECK(harness.saved_permissions(PLUGIN).process == std::vector<std::string>{R"(C:\Program)"});

    // Approving saves that exact command line beside the unrelated legacy grant.
    harness.approve = [](const Prompt& prompt) {
        return prompt.targets ==
               std::vector<std::string>{R"(CommandLine:C:\Program Files\ToolA\a.exe --one)"};
    };
    harness.prompts.clear();
    python.run_as(PLUGIN, R"PY(
approved_outcome = raise_popen(None, tool_a_one)
)PY");
    CHECK(python.str("approved_outcome") == "ok");
    CHECK(describe(harness.prompts) ==
          "subprocess.Popen: CommandLine:C:\\Program Files\\ToolA\\a.exe --one\n");
    CHECK(harness.saved_permissions(PLUGIN).process ==
          std::vector<std::string>{R"(C:\Program)", R"(CommandLine:C:\Program Files\ToolA\a.exe --one)"});

    // The same exact command line reuses the saved exact grant.
    harness.prompts.clear();
    python.run_as(PLUGIN, R"PY(
repeat_outcome = raise_popen(None, tool_a_one)
)PY");
    CHECK(python.str("repeat_outcome") == "ok");
    CHECK(describe(harness.prompts).empty());

    // The saved exact grant covers neither changed arguments nor the same arguments on another
    // executable, and the legacy C:\Program grant does not cover either exact command line.
    harness.approve = [](const Prompt&) { return false; };
    harness.prompts.clear();
    python.run_as(PLUGIN, R"PY(
changed_arguments_outcome = raise_popen(None, r"C:\Program Files\ToolA\a.exe --two")
other_executable_outcome = raise_popen(None, r"C:\Program Files\ToolB\b.exe --one")
)PY");
    CHECK(python.str("changed_arguments_outcome") == "denied");
    CHECK(python.str("other_executable_outcome") == "denied");
    CHECK(describe(harness.prompts) ==
          "subprocess.Popen: CommandLine:C:\\Program Files\\ToolA\\a.exe --two\n"
          "subprocess.Popen: CommandLine:C:\\Program Files\\ToolB\\b.exe --one\n");

    // A quoted executable and an explicit executable remain argument-insensitive grants.
    PluginPermissions permissions;
    permissions.process = {R"(C:\Program Files\Tool\tool.exe)", R"(D:\other\tool.exe)"};
    harness.save_permissions(PLUGIN, permissions);
    harness.prompts.clear();
    python.run_as(PLUGIN, R"PY(
quoted_outcome = raise_popen(None, r'"C:\Program Files\Tool\tool.exe" --changed')
explicit_outcome = raise_popen(r"D:\other\tool.exe", r'"C:\Program Files\Tool\tool.exe" --changed')
)PY");
    CHECK(python.str("quoted_outcome") == "ok");
    CHECK(python.str("explicit_outcome") == "ok");
    CHECK(describe(harness.prompts).empty());
}

TEST_CASE("A Windows command line grant names one quoted executable, not another path", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    // The event _winapi.CreateProcess raises, with application_name left out as it may be, so no
    // process is created. The quoted paths contain spaces, which a whitespace split would cut.
    python.run_as(PLUGIN, R"PY(
def raise_create_process(application_name, command_line):
    try:
        sys.audit("_winapi.CreateProcess", application_name, command_line, None)
        return "ok"
    except PermissionError:
        return "denied"

first_outcome = raise_create_process(None, r'"C:\Program Files\ToolA\a.exe" --one')
other_outcome = raise_create_process(None, r'"C:\Program Files\ToolB\b.exe" --one')
explicit_outcome = raise_create_process(r"D:\tools\other.exe", r'"C:\Program Files\ToolA\a.exe" --one')
)PY");
    CHECK(python.str("first_outcome") == "denied");
    CHECK(python.str("other_outcome") == "denied");
    CHECK(python.str("explicit_outcome") == "denied");
    CHECK(describe(harness.prompts) ==
          "_winapi.CreateProcess: C:\\Program Files\\ToolA\\a.exe\n"
          "_winapi.CreateProcess: C:\\Program Files\\ToolB\\b.exe\n"
          "_winapi.CreateProcess: D:\\tools\\other.exe\n");

    // A grant for one quoted path covers that executable with other arguments, never another path.
    harness.approve = [](const Prompt& prompt) {
        return prompt.targets == std::vector<std::string>{R"(C:\Program Files\ToolA\a.exe)"};
    };
    harness.prompts.clear();
    python.run_as(PLUGIN, R"PY(
repeat_outcome = raise_create_process(None, r'"C:\Program Files\ToolA\a.exe" --two')
distinct_outcome = raise_create_process(None, r'"C:\Program Files\ToolB\b.exe" --one')
)PY");
    CHECK(python.str("repeat_outcome") == "ok");
    CHECK(python.str("distinct_outcome") == "denied");
    CHECK(describe(harness.prompts) ==
          "_winapi.CreateProcess: C:\\Program Files\\ToolA\\a.exe\n"
          "_winapi.CreateProcess: C:\\Program Files\\ToolB\\b.exe\n");
    CHECK(harness.saved_permissions(PLUGIN).process ==
          std::vector<std::string>{R"(C:\Program Files\ToolA\a.exe)"});

    // The saved grant covers the same executable again, still without spanning to ToolB.
    harness.prompts.clear();
    python.run_as(PLUGIN, R"PY(
saved_outcome = raise_create_process(None, r'"C:\Program Files\ToolA\a.exe" --three')
other_saved_outcome = raise_create_process(None, r'"C:\Program Files\ToolB\b.exe" --two')
)PY");
    CHECK(python.str("saved_outcome") == "ok");
    CHECK(python.str("other_saved_outcome") == "denied");
    CHECK(describe(harness.prompts) == "_winapi.CreateProcess: C:\\Program Files\\ToolB\\b.exe\n");
}

TEST_CASE("An os.system grant names the shell and covers every command", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);
    const std::vector<std::string> shells = python.strings("SYSTEM_SHELLS");

    // Raising the event directly exercises the targets the hook reads without running the command.
    // The grants name the shells, not the command string's first token, on every platform.
    python.run_as(PLUGIN, R"PY(
def raise_system(command):
    try:
        sys.audit("os.system", command)
        return "ok"
    except PermissionError:
        return "denied"

plain_outcome = raise_system("plain-tool --one")
quoted_outcome = raise_system(r'"C:\Program Files\ToolA\a.exe" --one')
other_outcome = raise_system("other-tool")
)PY");
    CHECK(python.str("plain_outcome") == "denied");
    CHECK(python.str("quoted_outcome") == "denied");
    CHECK(python.str("other_outcome") == "denied");
    CHECK(describe(harness.prompts) ==
          describe_system_prompt(shells) + describe_system_prompt(shells) + describe_system_prompt(shells));

    // Approving the shells covers every command string, arguments included.
    harness.approve = [](const Prompt&) { return true; };
    harness.prompts.clear();
    python.run_as(PLUGIN, R"PY(
repeat_outcome = raise_system("plain-tool --two")
)PY");
    CHECK(python.str("repeat_outcome") == "ok");
    REQUIRE(harness.prompts.size() == 1);
    CHECK(harness.prompts.front().targets == shells);
    CHECK(harness.saved_permissions(PLUGIN).process == shells);

    harness.prompts.clear();
    python.run_as(PLUGIN, R"PY(
quoted_repeat_outcome = raise_system(r'"C:\Program Files\ToolB\b.exe" --flag')
other_repeat_outcome = raise_system("third-tool --three")
)PY");
    CHECK(python.str("quoted_repeat_outcome") == "ok");
    CHECK(python.str("other_repeat_outcome") == "ok");
    CHECK(describe(harness.prompts).empty());
}

#ifdef _WIN32
TEST_CASE("An os.system grant for COMSPEC alone does not authorize the fallback shell", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    // Pin the configured interpreter, so the case does not depend on the machine's COMSPEC.
    python.run_as(PLUGIN, R"PY(
saved_comspec = os.environ.get("COMSPEC")
NARROW_SHELL = r"C:\narrow\narrow-shell.exe"
os.environ["COMSPEC"] = NARROW_SHELL

def raise_system(command):
    try:
        sys.audit("os.system", command)
        return "ok"
    except PermissionError:
        return "denied"
)PY");
    const std::string narrow_shell = python.str("NARROW_SHELL");
    const std::vector<std::string> both_shells{narrow_shell, "cmd.exe"};

    // The runtime falls back to cmd.exe when it cannot open COMSPEC, so the configured
    // interpreter's grant alone leaves the event unresolved by the fallback shell.
    PluginPermissions permissions;
    permissions.process = {narrow_shell};
    harness.save_permissions(PLUGIN, permissions);
    python.run_as(PLUGIN, "narrow_outcome = raise_system('anything')");
    CHECK(python.str("narrow_outcome") == "denied");
    CHECK(describe(harness.prompts) == "os.system: cmd.exe\n");
    CHECK(harness.saved_permissions(PLUGIN).process == std::vector<std::string>{narrow_shell});

    // One approval saves the missing fallback interpreter, so the next run does not ask again.
    harness.approve = [](const Prompt&) { return true; };
    harness.prompts.clear();
    python.run_as(PLUGIN, "approved_outcome = raise_system('anything')");
    CHECK(python.str("approved_outcome") == "ok");
    CHECK(harness.prompts.size() == 1);
    if (harness.prompts.size() == 1)
        CHECK(harness.prompts.front().targets == std::vector<std::string>{"cmd.exe"});
    CHECK(harness.saved_permissions(PLUGIN).process == both_shells);

    harness.prompts.clear();
    python.run_as(PLUGIN, "repeat_outcome = raise_system('anything else')");
    CHECK(python.str("repeat_outcome") == "ok");
    CHECK(describe(harness.prompts).empty());

    // A COMSPEC that is the bare fallback name names one interpreter, whatever its casing.
    harness.save_permissions(PLUGIN, {});
    harness.approve = [](const Prompt&) { return false; };
    python.run_as(PLUGIN, R"PY(
os.environ["COMSPEC"] = "CmD.eXe"
bare_outcome = raise_system("anything")
)PY");
    CHECK(python.str("bare_outcome") == "denied");
    CHECK(describe(harness.prompts) == "os.system: cmd.exe\n");
    CHECK(harness.saved_permissions(PLUGIN).process.empty());

    python.run_as(PLUGIN, R"PY(
if saved_comspec is None:
    os.environ.pop("COMSPEC", None)
else:
    os.environ["COMSPEC"] = saved_comspec
)PY");
}
#endif

TEST_CASE("A finalizer run when a root frame is let go is judged as the plugin without a prompt", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);
    PluginPermissions permissions = http_grants({python.str("URL_A")});
    permissions.fs_read           = {python.str("APPROVED")};
    harness.save_permissions(PLUGIN, permissions);
    PluginAuditManager& mgr = PluginAuditManager::instance();

    // Once the request returns, only its held frame keeps the request object alive, and the
    // request's finalizer reads a file the plugin was never granted.
    python.run_as(PLUGIN, "fetch(TrackedRequest(URL_A))");
    CHECK(python.strings("finalizer_outcomes") == std::vector<std::string>{"denied"}); // at scope end
    CHECK(describe(harness.prompts).empty());
    CHECK_FALSE(mgr.audit_denial_pending());

    python.exec("finalizer_outcomes.clear()");
    python.run_as(PLUGIN, R"PY(
fetch(TrackedRequest(URL_A))
open(APPROVED).close()  # the next event lets the returned request's frame go
outcomes_before_scope_end = list(finalizer_outcomes)
)PY");
    CHECK(python.strings("outcomes_before_scope_end") == std::vector<std::string>{"denied"});
    CHECK(describe(harness.prompts).empty());

    // The finalizer's denial during another event's decision leaves that event's own denial intact.
    python.exec("finalizer_outcomes.clear()");
    python.run_as(PLUGIN, R"PY(
fetch(TrackedRequest(URL_A))
try:
    os.listdir(OUTSIDE)
    outer_error = ""
except PermissionError as error:
    outer_error = str(error)
)PY");
    CHECK(python.strings("finalizer_outcomes") == std::vector<std::string>{"denied"});
    CHECK_THAT(python.str("outer_error"), Catch::Matchers::ContainsSubstring("os.listdir"));
    CHECK(describe(harness.prompts) == "os.listdir: " + python.str("OUTSIDE") + "\n");
    AuditViolation violation;
    REQUIRE(mgr.last_violation(violation));
    CHECK(violation.event_name == "os.listdir");
    CHECK(mgr.audit_denial_pending());
}

TEST_CASE("A finalizer hardlink to an ungranted source is denied without prompting during frame release", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);
    PluginPermissions permissions = http_grants({python.str("URL_A")});
    harness.save_permissions(PLUGIN, permissions);
    PluginAuditManager& mgr = PluginAuditManager::instance();

    python.exec(R"PY(
FINALIZER_LINK_SOURCE = os.path.join(OUTSIDE, "finalizer-link-source.txt")
with open(FINALIZER_LINK_SOURCE, "w") as f:
    f.write("x")
FINALIZER_LINK_DEST = os.path.join(OUTSIDE, "finalizer-link-dest.txt")
finalizer_link_outcomes = []

class LinkTrackedRequest(urllib.request.Request):
    def __del__(self):
        finalizer_link_outcomes.append(attempt(lambda: os.link(FINALIZER_LINK_SOURCE, FINALIZER_LINK_DEST)))
)PY");

    // The request is kept alive by the approved fetch's root frame. When the frame is released, its
    // finalizer tries to hardlink a source that was never granted. Releasing frames runs finalizers
    // where a modal cannot open, so the source check must fail closed without prompting.
    python.run_as(PLUGIN, "fetch(LinkTrackedRequest(URL_A))");

    CHECK(python.strings("finalizer_link_outcomes") == std::vector<std::string>{"denied"});
    CHECK(describe(harness.prompts).empty());
    CHECK_FALSE(fs::exists(fs::path(python.str("FINALIZER_LINK_DEST"))));
    CHECK_FALSE(mgr.audit_denial_pending());
}

TEST_CASE("A nested audit scope does not see the outer scope's running operations", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin("PluginA");
    harness.add_plugin("PluginB");
    py::gil_scoped_acquire gil;
    PythonSide python(harness);
    PluginPermissions permissions;
    permissions.fs_read = {python.str("TREE")};
    harness.save_permissions("PluginA", permissions);

    {
        // Two walks plugin A started and suspended after listing the tree itself.
        ScopedPluginAuditContext plugin_a("PluginA");
        python.exec("first = os.walk(TREE)\nnext(first)\nsecond = os.walk(TREE)\nnext(second)");
        {
            // Plugin B resuming one of them is asked for each subdirectory, as plugin B.
            ScopedPluginAuditContext plugin_b("PluginB");
            python.exec("listed_by_b = len(list(first))");
        }
        // Back in plugin A's scope, its walks are covered again.
        python.exec("listed_by_a = len(list(second))");
    }

    CHECK(python.globals["listed_by_b"].cast<int>() == 0);
    CHECK(python.globals["listed_by_a"].cast<int>() == 2);
    REQUIRE(harness.prompts.size() == 2);
    for (const Prompt& prompt : harness.prompts) {
        CHECK(prompt.plugin == "PluginB");
        CHECK(prompt.event == "os.scandir");
    }
}

TEST_CASE("Root frames held when a capability call ends are let go with the GIL, as the plugin", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.approve = [](const Prompt& prompt) { return prompt.event == "os.walk"; };
    py::gil_scoped_acquire gil;
    PythonSide python(harness);
    (void) PythonPluginBridge::instance(); // force the embedded module registration into the binary
    python.globals["orca"] = py::module_::import("orca");

    // walk_and_drop() leaves a returned walk's frame held by the audit scope alone, with a marker
    // in its locals; the frame is let go when the capability call ends, which runs the marker's
    // finalizer.
    python.exec(R"PY(
class Marker:
    def __init__(self, action=None):
        self.action = action
    def __del__(self):
        finalized.append(self.action() if self.action else "ran")

finalized = []

def walk_and_drop(marker):
    walker = os.walk(TREE, onerror=marker)
    next(walker)
    del walker

class ConfigCapability(orca.PythonPluginBase):
    def get_name(self):
        return "config_cap"
    def get_default_config(self):
        walk_and_drop(Marker())
        return {"finalized_during_call": len(finalized)}
    def has_config_ui(self):
        walker = os.walk(TREE, onerror=Marker(lambda: attempt(lambda: open(PRIVATE).close())))
        next(walker)
        attempt(lambda: os.listdir(OUTSIDE))  # denied: the trampoline clears the plugin key
        del walker  # the walk's frame is now held by the audit scope alone
        raise RuntimeError("the capability fails after a denial")

class TicketAgent(orca.printer_agent.PrinterAgentBase):
    def get_name(self):
        return "ticket_agent"
    def request_bind_ticket(self):
        walk_and_drop(Marker())
        return (0, "ticket")
)PY");

    py::object config_instance = python.globals["ConfigCapability"]();
    auto       config_cap      = config_instance.cast<std::shared_ptr<PluginCapabilityInterface>>();
    config_cap->set_audit_plugin_key("GilPlugin");
    config_cap->set_resolved_identity("config_cap", PluginCapabilityType::Script);

    py::object agent_instance = python.globals["TicketAgent"]();
    auto       agent_cap      = agent_instance.cast<std::shared_ptr<PluginCapabilityInterface>>();
    agent_cap->set_audit_plugin_key("GilPlugin");
    agent_cap->set_resolved_identity("ticket_agent", PluginCapabilityType::PrinterConnection);
    auto agent = std::dynamic_pointer_cast<IPrinterAgent>(agent_cap);
    REQUIRE(agent);

    // A host call arrives without the GIL; both calls take it before their audit scope.
    nlohmann::json config;
    int            ticket_result = -1;
    std::string    ticket;
    {
        py::gil_scoped_release released;
        config        = config_cap->get_default_config();
        ticket_result = agent->request_bind_ticket(&ticket);
    }
    CHECK(config == nlohmann::json{{"finalized_during_call", 0}});
    CHECK(ticket_result == 0);
    CHECK(ticket == "ticket");
    CHECK(python.strings("finalized") == std::vector<std::string>{"ran", "ran"});

    // On the trampoline's denial path the frame is let go as the plugin, though the ambient key
    // was cleared: the finalizer's read is audited and denied without a prompt.
    python.exec("finalized.clear()");
    harness.prompts.clear();
    CHECK_THROWS_AS(config_cap->has_config_ui(), py::error_already_set);
    CHECK(python.strings("finalized") == std::vector<std::string>{"denied"});
    CHECK(describe(harness.prompts) == "os.walk: " + python.str("TREE") + "\nos.listdir: " + python.str("OUTSIDE") + "\n");
}

TEST_CASE("Stdlib code is recognized through a symlinked Python home", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    py::gil_scoped_acquire gil;

    const py::dict paths   = py::module_::import("sysconfig").attr("get_paths")();
    const fs::path stdlib  = py::str(paths["stdlib"]).cast<std::string>();
    const fs::path purelib = py::str(paths["purelib"]).cast<std::string>();
    const fs::path link    = harness.outside.path() / "linked-stdlib";
    boost::system::error_code ec;
    fs::create_directory_symlink(stdlib, link, ec);
    if (ec)
        SKIP("Cannot create a directory symlink here: " + ec.message());

    CHECK(PluginAuditManager::is_stdlib_file((stdlib / "os.py").string()));
    CHECK(PluginAuditManager::is_stdlib_file((link / "urllib" / "request.py").string()));
    CHECK(PluginAuditManager::is_stdlib_file("<frozen os>"));
    CHECK_FALSE(PluginAuditManager::is_stdlib_file((purelib / "requests" / "api.py").string()));
    CHECK_FALSE(PluginAuditManager::is_stdlib_file((link / "site-packages" / "requests" / "api.py").string()));
    CHECK_FALSE(PluginAuditManager::is_stdlib_file((harness.outside.path() / "plugin.py").string()));
    CHECK_FALSE(PluginAuditManager::is_stdlib_file("<string>"));
}

TEST_CASE("A plugin may read the CPython stdlib without a prompt, but not site-packages or for writing", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    const py::dict paths   = py::module_::import("sysconfig").attr("get_paths")();
    const fs::path stdlib  = py::str(paths["stdlib"]).cast<std::string>();
    const fs::path purelib = py::str(paths["purelib"]).cast<std::string>();
    // os.py ships with every CPython installation. A site-packages path is third-party code even
    // when the directory is empty: the prompt must come before the missing file is noticed.
    const fs::path stdlib_file = stdlib / "os.py";
    REQUIRE(fs::exists(stdlib_file));

    python.globals["STDLIB_FILE"]       = stdlib_file.string();
    python.globals["SITE_PACKAGE_FILE"] = (purelib / "regression_probe.py").string();
    python.run_as(PLUGIN, R"PY(
read_outcome = attempt(lambda: open(STDLIB_FILE).read())
write_outcome = attempt(lambda: open(STDLIB_FILE, "r+").close())
site_packages_outcome = attempt(lambda: open(SITE_PACKAGE_FILE).close())
)PY");

    CHECK(python.str("read_outcome") == "ok");
    CHECK(python.str("write_outcome") == "denied");
    CHECK(python.str("site_packages_outcome") == "denied");
    // Only the write and the site-packages read are asked about; the stdlib read is implicit.
    const std::string write_prompt         = "open: " + python.str("STDLIB_FILE") + "\n";
    const std::string site_packages_prompt = "open: " + python.str("SITE_PACKAGE_FILE") + "\n";
    CHECK(describe(harness.prompts) == write_prompt + site_packages_prompt);
}

#ifdef _WIN32
TEST_CASE("A real _winapi.CreateProcess audit event reports its command line as text", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    // subprocess reaches this event on Windows. CPython 3.12.13 raised it with PySys_Audit's "uuu"
    // format, reading the command-line object as a wide string, so audit hooks received garbage
    // instead of the text subprocess passed. The deps patch raises "uOu" (as upstream does,
    // python/cpython#159237) until the pinned release carries the fix.
    //
    // The event is raised before the arguments are used, so a rejected environment mapping ends
    // the call after the hook saw the event, without reaching CreateProcessW. Run outside a
    // plugin scope, so the hook below is reached: the plugin audit hook aborts the event when it
    // denies, and a real prompt is asserted separately at the end.
    python.exec(R"PY(
WINAPI_AUDIT_CALLS = []

def record_winapi_audit(event, args):
    if event == "_winapi.CreateProcess":
        WINAPI_AUDIT_CALLS.append(args)

sys.addaudithook(record_winapi_audit)

DENIED_EXECUTABLE = os.path.join(OUTSIDE, "Program Files", "audit-denied.exe")
COMMAND_LINE = '"' + DENIED_EXECUTABLE + '" --flag'
try:
    _winapi.CreateProcess(None, COMMAND_LINE, None, None, False, 0, object(), None,
                          subprocess.STARTUPINFO())
    audit_arguments_outcome = "started"
except TypeError:
    audit_arguments_outcome = "stopped"
)PY");
    CHECK(python.str("audit_arguments_outcome") == "stopped");
    INFO("recorded audit arguments: " << python.str("WINAPI_AUDIT_CALLS"));
    CHECK(python.eval("WINAPI_AUDIT_CALLS == [(None, COMMAND_LINE, None)]") == "True");

    // In a plugin scope the same call is prompted and denied. The quoted executable is the
    // permission target; its arguments are not part of the grant.
    python.run_as(PLUGIN, R"PY(
create_process_outcome = attempt(lambda: _winapi.CreateProcess(
    None, COMMAND_LINE, None, None, False, 0, None, None, subprocess.STARTUPINFO()))
)PY");
    CHECK(python.str("create_process_outcome") == "denied");
    CHECK(describe(harness.prompts) == "_winapi.CreateProcess: " + python.str("DENIED_EXECUTABLE") + "\n");
}
#endif

TEST_CASE("A filesystem wildcard grant covers the folder and its descendants at any depth", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    python.exec(R"PY(
DEEP_FILE = os.path.join(TREE, "one", "deep", "deeper", "f.txt")
os.makedirs(os.path.dirname(DEEP_FILE))
with open(DEEP_FILE, "w") as f:
    f.write("x")
NESTED_FILE = os.path.join(TREE, "one", "f.txt")
SIBLING = TREE + "-sibling"
SIBLING_FILE = os.path.join(SIBLING, "f.txt")
os.makedirs(SIBLING)
with open(SIBLING_FILE, "w") as f:
    f.write("x")
# Lexically under TREE, but ".." resolves outside the granted folder.
ESCAPE_FILE = os.path.join(TREE, "..", "private.txt")
)PY");

    const std::string tree = python.str("TREE");
    PluginPermissions permissions;
    permissions.fs_read = {tree + "/*"};
    harness.save_permissions(PLUGIN, permissions);

    python.run_as(PLUGIN, R"PY(
deep_outcome = attempt(lambda: open(DEEP_FILE).read())
root_outcome = attempt(lambda: os.listdir(TREE))
sibling_outcome = attempt(lambda: open(SIBLING_FILE).read())
escape_outcome = attempt(lambda: open(ESCAPE_FILE).read())
)PY");

    // The tree grant covers the folder itself (listing it) and a file two directories below it.
    CHECK(python.str("deep_outcome") == "ok");
    CHECK(python.str("root_outcome") == "ok");
    // A path sharing the folder's prefix is not a descendant, and ".." is resolved before the
    // comparison, so neither escapes the grant; both are asked about.
    CHECK(python.str("sibling_outcome") == "denied");
    CHECK(python.str("escape_outcome") == "denied");
    const std::string sibling_prompt = "open: " + python.str("SIBLING_FILE") + "\n";
    const std::string escape_prompt  = "open: " + python.str("ESCAPE_FILE") + "\n";
    CHECK(describe(harness.prompts) == sibling_prompt + escape_prompt);

    // Only the final component may be a wildcard: a "*" inside it is an ordinary character.
    permissions.fs_read = {tree + "/*.txt"};
    harness.save_permissions(PLUGIN, permissions);
    harness.prompts.clear();
    python.run_as(PLUGIN, "literal_outcome = attempt(lambda: open(NESTED_FILE).read())");
    CHECK(python.str("literal_outcome") == "denied");
    const std::string literal_prompt = "open: " + python.str("NESTED_FILE") + "\n";
    CHECK(describe(harness.prompts) == literal_prompt);
}

TEST_CASE("A tree grant compares canonical paths on both sides", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    // A folder outside TREE, reached through a symlink planted inside it.
    const fs::path tree        = python.str("TREE");
    const fs::path outside_dir = harness.outside.path() / "symlink-target";
    const fs::path link        = tree / "escape";
    fs::create_directories(outside_dir);
    std::ofstream((outside_dir / "outside.txt").string(), std::ios::binary) << "x";

    boost::system::error_code ec;
    fs::create_directory_symlink(outside_dir, link, ec);
    if (ec)
        SKIP("Cannot create a directory symlink here: " + ec.message());

    PluginPermissions permissions;
    permissions.fs_read = {tree.string() + "/*"};
    harness.save_permissions(PLUGIN, permissions);

    const std::string escaped_file = (link / "outside.txt").string();
    python.globals["ESCAPED_FILE"] = escaped_file;
    python.run_as(PLUGIN, "escaped_outcome = attempt(lambda: open(ESCAPED_FILE).read())");

    // The target canonicalizes outside the canonical grant root, so the link does not inherit
    // the tree grant. The read/write wildcard test covers a non-existing write tail.
    CHECK(python.str("escaped_outcome") == "denied");
    CHECK(describe(harness.prompts) == "open: " + escaped_file + "\n");

    // A grant rooted at the link applies to what the link points at, not just to its path.
    permissions.fs_read = {link.string() + "/*"};
    harness.save_permissions(PLUGIN, permissions);
    harness.prompts.clear();
    python.globals["TARGET_FILE"] = (outside_dir / "outside.txt").string();
    python.run_as(PLUGIN, "via_target_outcome = attempt(lambda: open(TARGET_FILE).read())");

    CHECK(python.str("via_target_outcome") == "ok");
    CHECK(describe(harness.prompts).empty());
}

TEST_CASE("A write through a dangling symlink inside a granted tree is denied", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    // Two links inside TREE whose targets do not exist yet, so both dangle:
    //  - "dangling" points outside the tree, the reported escape;
    //  - "dangling-inside" points at a not-yet-created file that would be inside the tree, to pin
    //    the fail-closed choice: a dangling link is rejected whatever its target.
    const fs::path tree          = python.str("TREE");
    const fs::path outside_file  = harness.outside.path() / "written-through-link.txt";
    const fs::path link          = tree / "dangling";
    const fs::path inside_target = tree / "not-created.txt";
    const fs::path inside_link   = tree / "dangling-inside";
    boost::system::error_code ec;
    fs::create_symlink(outside_file, link, ec);
    if (ec)
        SKIP("Cannot create a file symlink here: " + ec.message());
    fs::create_symlink(inside_target, inside_link, ec);
    if (ec)
        SKIP("Cannot create a file symlink here: " + ec.message());

    PluginPermissions permissions;
    permissions.fs_readwrite = {tree.string() + "/*"};
    harness.save_permissions(PLUGIN, permissions);

    python.globals["DANGLING_LINK"]        = link.string();
    python.globals["DANGLING_INSIDE_LINK"] = inside_link.string();
    python.run_as(PLUGIN, R"PY(
inside_outcome = attempt(lambda: open(os.path.join(TREE, "inside.txt"), "w").close())
outcome = attempt(lambda: open(DANGLING_LINK, "w").close())
inside_link_outcome = attempt(lambda: open(DANGLING_INSIDE_LINK, "w").close())
)PY");

    // The wildcard still covers an ordinary new file in the tree. A dangling final symlink is not
    // covered: the grant describes the path the plugin spelled, while a write through the link
    // would land at its target, so the prompt is refused and neither destination is created.
    CHECK(python.str("inside_outcome") == "ok");
    CHECK(python.str("outcome") == "denied");
    CHECK(python.str("inside_link_outcome") == "denied");
    CHECK(describe(harness.prompts) == "open: " + link.string() + "\nopen: " + inside_link.string() + "\n");
    CHECK_FALSE(fs::exists(outside_file));
    CHECK_FALSE(fs::exists(inside_target));
}

TEST_CASE("A saved exact file grant covers an equivalent spelling of the same file", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    // The granted file, a file beside it, a lookalike name, the not-yet-existing target of a
    // dangling link, and the not-yet-existing target of a write.
    python.exec(R"PY(
FILE = os.path.join(OUTSIDE, "approved-file.txt")
OTHER = os.path.join(OUTSIDE, "other-file.txt")
LOOKALIKE = os.path.join(OUTSIDE, "approved-file.txt.bak")
NOT_CREATED = os.path.join(OUTSIDE, "not-created.txt")
NEW_FILE = os.path.join(OUTSIDE, "new-file.txt")
ESCAPE_PATH = os.path.join(OUTSIDE, "sub", "..", "other-file.txt")
os.makedirs(os.path.join(OUTSIDE, "sub"))
for path in (FILE, OTHER, LOOKALIKE):
    with open(path, "w") as f:
        f.write("x")
)PY");

    const fs::path            outside     = harness.outside.path();
    const fs::path            file        = python.str("FILE");
    const fs::path            alias       = outside / "alias.txt";
    const fs::path            other_alias = outside / "other-alias.txt";
    const fs::path            dangling    = outside / "dangling.txt";
    boost::system::error_code ec;
    // A link to the granted file, a link to a different file, and a link whose target does not
    // exist yet.
    fs::create_symlink(file, alias, ec);
    if (ec)
        SKIP("Cannot create a file symlink here: " + ec.message());
    fs::create_symlink(outside / "other-file.txt", other_alias, ec);
    if (ec)
        SKIP("Cannot create a file symlink here: " + ec.message());
    fs::create_symlink(outside / "not-created.txt", dangling, ec);
    if (ec)
        SKIP("Cannot create a file symlink here: " + ec.message());

    python.globals["ALIAS"]      = alias.string();
    python.globals["OTHER_LINK"] = other_alias.string();
    python.globals["DANGLING"]   = dangling.string();

    PluginPermissions permissions;
    permissions.fs_read      = {python.str("FILE")};
    permissions.fs_readwrite = {python.str("NOT_CREATED"), python.str("NEW_FILE")};
    harness.save_permissions(PLUGIN, permissions);

    // A relative target is resolved against the working directory the open resolves in, so the
    // grant matches the file it names even though the audit target carries no directory.
    const fs::path original_cwd = fs::current_path();
    fs::current_path(outside, ec);
    REQUIRE_FALSE(ec);
    python.run_as(PLUGIN, R"PY(
relative_read_outcome = attempt(lambda: open("approved-file.txt").read())
relative_write_outcome = attempt(lambda: open("new-file.txt", "w").close())
)PY");
    fs::current_path(original_cwd, ec);
    REQUIRE_FALSE(ec);

    python.run_as(PLUGIN, R"PY(
verbatim_outcome = attempt(lambda: open(FILE).read())
dot_outcome = attempt(lambda: open(os.path.join(OUTSIDE, ".", "approved-file.txt")).read())
updown_outcome = attempt(lambda: open(os.path.join(OUTSIDE, "sub", "..", "approved-file.txt")).read())
alias_outcome = attempt(lambda: open(ALIAS).read())
other_outcome = attempt(lambda: open(OTHER).read())
escape_outcome = attempt(lambda: open(ESCAPE_PATH).read())
other_link_outcome = attempt(lambda: open(OTHER_LINK).read())
lookalike_outcome = attempt(lambda: open(LOOKALIKE).read())
dangling_outcome = attempt(lambda: open(DANGLING, "w").close())
)PY");

    CHECK(python.str("verbatim_outcome") == "ok");
    CHECK(python.str("relative_read_outcome") == "ok");
    CHECK(python.str("relative_write_outcome") == "ok");
    CHECK(python.str("dot_outcome") == "ok");
    CHECK(python.str("updown_outcome") == "ok");
    CHECK(python.str("alias_outcome") == "ok");

    // Full path equality, not containment: a sibling, a lookalike name, a link to another file,
    // and a ".." spelling that lands on another file are all asked about.
    CHECK(python.str("other_outcome") == "denied");
    CHECK(python.str("escape_outcome") == "denied");
    CHECK(python.str("other_link_outcome") == "denied");
    CHECK(python.str("lookalike_outcome") == "denied");
    // Fail closed on a dangling final link: the write would create its target, not the path the
    // grant names, even though that target itself carries a read/write grant.
    CHECK(python.str("dangling_outcome") == "denied");
    CHECK_FALSE(fs::exists(outside / "not-created.txt"));

    const std::string open_prompt = "open: ";
    const std::string expected_prompts =
        open_prompt + python.str("OTHER") + "\n" + open_prompt + python.str("ESCAPE_PATH") + "\n" +
        open_prompt + python.str("OTHER_LINK") + "\n" + open_prompt + python.str("LOOKALIKE") + "\n" +
        open_prompt + python.str("DANGLING") + "\n";
    CHECK(describe(harness.prompts) == expected_prompts);

#ifdef _WIN32
    // Windows compares path names case-insensitively and accepts either separator, so a different
    // spelling of the granted file matches without another prompt.
    std::string forward_slashes;
    for (const char c : file.string())
        forward_slashes += c == '\\' ? '/' : c;
    python.globals["UPPERCASE_FILE"]     = (file.parent_path() / "APPROVED-FILE.TXT").string();
    python.globals["FORWARD_SLASH_FILE"] = forward_slashes;
    harness.prompts.clear();
    python.run_as(PLUGIN, R"PY(
uppercase_outcome = attempt(lambda: open(UPPERCASE_FILE).read())
forward_slash_outcome = attempt(lambda: open(FORWARD_SLASH_FILE).read())
)PY");
    CHECK(python.str("uppercase_outcome") == "ok");
    CHECK(python.str("forward_slash_outcome") == "ok");
    CHECK(describe(harness.prompts).empty());
#endif

    // A grant saved as a link applies to the file it points at, not just to the link's spelling.
    permissions.fs_read      = {alias.string()};
    permissions.fs_readwrite.clear();
    harness.save_permissions(PLUGIN, permissions);
    harness.prompts.clear();
    python.run_as(PLUGIN, "grant_via_link_outcome = attempt(lambda: open(FILE).read())");
    CHECK(python.str("grant_via_link_outcome") == "ok");
    CHECK(describe(harness.prompts).empty());

    // An exact grant names one file: it does not become a tree grant, so it does not cover what
    // lives inside the folder it names.
    permissions.fs_read = {python.str("OUTSIDE")};
    harness.save_permissions(PLUGIN, permissions);
    harness.prompts.clear();
    python.run_as(PLUGIN, "inside_granted_folder_outcome = attempt(lambda: open(FILE).read())");
    CHECK(python.str("inside_granted_folder_outcome") == "denied");
    CHECK(describe(harness.prompts) == open_prompt + python.str("FILE") + "\n");

    // A grant that names a dangling link also covers only its verbatim spelling: an equivalent
    // spelling of the target the write would create is not matched by equivalence.
    permissions.fs_read.clear();
    permissions.fs_readwrite = {dangling.string()};
    harness.save_permissions(PLUGIN, permissions);
    harness.prompts.clear();
    python.run_as(PLUGIN, "through_dangling_grant_outcome = attempt(lambda: open(NOT_CREATED, 'w').close())");
    CHECK(python.str("through_dangling_grant_outcome") == "denied");
    CHECK(describe(harness.prompts) == open_prompt + python.str("NOT_CREATED") + "\n");
    CHECK_FALSE(fs::exists(outside / "not-created.txt"));
}

TEST_CASE("A saved exact file grant covers the file's 8.3 alias without another prompt", "[audit][Python]")
{
#ifndef _WIN32
    SKIP("Windows 8.3 short names exist only on Windows");
#else
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    // No symlink is involved, so this check does not depend on the privilege to create one. The
    // file name and the temp folder both need their long spelling, so the alias differs from the
    // grant even where the volume names the temp root short.
    python.exec(R"PY(
GRANTED_FILE = os.path.join(OUTSIDE, "short-name-granted-file.txt")
with open(GRANTED_FILE, "w") as f:
    f.write("x")
)PY");

    const fs::path file  = python.str("GRANTED_FILE");
    const fs::path alias = short_name_alias(file);
    if (alias.empty())
        SKIP("This volume does not expose an 8.3 alias for the granted file");

    PluginPermissions permissions;
    permissions.fs_read = {file.string()};
    harness.save_permissions(PLUGIN, permissions);

    python.globals["ALIAS_FILE"] = alias.string();
    python.run_as(PLUGIN, "alias_outcome = attempt(lambda: open(ALIAS_FILE).read())");

    // The grant was saved under the long spelling; reading through the alias must not ask again.
    CHECK(python.str("alias_outcome") == "ok");
    CHECK(describe(harness.prompts).empty());
#endif
}

TEST_CASE("A saved exact file grant covers an equivalent spelling in a permission request", "[audit]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);

    const fs::path file = harness.outside.path() / "requested-file.txt";
    PluginPermissions stored;
    stored.fs_read = {file.string()};
    harness.save_permissions(PLUGIN, stored);

    std::vector<std::string> prompted;
    PluginAuditManager::set_permission_message_prompt_for_testing(
        [&prompted](const std::string& message) {
            prompted.push_back(message);
            return false;
        });
    PluginAuditManager& mgr = PluginAuditManager::instance();

    // An absolute declaration that is a different spelling of the granted path is already covered,
    // so it is neither shown nor appended as a duplicate grant.
    PluginPermissions dotted;
    dotted.fs_read = {file.parent_path().string() + "/./" + file.filename().string()};
    CHECK(mgr.request_permissions(PLUGIN, dotted));
    CHECK(prompted.empty());
    CHECK(harness.saved_permissions(PLUGIN).fs_read == std::vector<std::string>{file.string()});

    // A relative declaration resolves against the directory the request is made in.
    const fs::path            original_cwd = fs::current_path();
    boost::system::error_code ec;
    fs::current_path(harness.outside.path(), ec);
    REQUIRE_FALSE(ec);
    PluginPermissions relative;
    relative.fs_read = {file.filename().string()};
    const bool relative_granted = mgr.request_permissions(PLUGIN, relative);
    fs::current_path(original_cwd, ec);
    REQUIRE_FALSE(ec);
    CHECK(relative_granted);
    CHECK(prompted.empty());

    // A different file still needs its own grant, so the equivalence above did not widen anything.
    PluginPermissions different;
    different.fs_read = {(harness.outside.path() / "different-file.txt").string()};
    CHECK_FALSE(mgr.request_permissions(PLUGIN, different));
    REQUIRE(prompted.size() == 1);
    CHECK(prompted.front().find("different-file.txt") != std::string::npos);
    PluginAuditManager::set_permission_message_prompt_for_testing(nullptr);
}

TEST_CASE("A relative wildcard permission request is shown and stored as an absolute grant", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);
    PluginAuditManager& mgr = PluginAuditManager::instance();

    // The plugin declares the grant relative to the working directory of the request, while the
    // audit hook reports absolute targets. Resolve against the directory the request is made in.
    const std::string relative_grant = "tree/*";
    PluginPermissions requested;
    requested.fs_readwrite = {relative_grant};

    const fs::path original_cwd = fs::current_path();
    boost::system::error_code ec;
    fs::current_path(harness.outside.path(), ec);
    REQUIRE_FALSE(ec);
    const fs::path request_dir = fs::current_path();

    std::string shown;
    PluginAuditManager::set_permission_message_prompt_for_testing(
        [&shown](const std::string& message) { shown = message; return true; });
    const bool granted = mgr.request_permissions(PLUGIN, requested);
    PluginAuditManager::set_permission_message_prompt_for_testing(nullptr);
    fs::current_path(original_cwd, ec);

    const std::string absolute_grant = (request_dir / "tree" / "*").string();

    REQUIRE(granted);
    // The prompt names the resolved scope, not the relative declaration.
    CHECK(shown.find(absolute_grant) != std::string::npos);
    CHECK(shown.find("\n" + relative_grant + "\n") == std::string::npos);
    CHECK(harness.saved_permissions(PLUGIN).fs_readwrite == std::vector<std::string>{absolute_grant});

    // The saved absolute grant is the effective one: an absolute target below the resolved
    // directory is covered, so the write is not asked about again.
    python.globals["RESOLVED_FILE"] = (harness.outside.path() / "tree" / "written.txt").string();
    python.run_as(PLUGIN, "outcome = attempt(lambda: open(RESOLVED_FILE, 'w').close())");
    CHECK(python.str("outcome") == "ok");
    CHECK(describe(harness.prompts).empty());
}

TEST_CASE("A create or delete tree declaration is stored as an absolute subtree grant", "[audit]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    PluginAuditManager& mgr = PluginAuditManager::instance();

    const fs::path folder = harness.outside.path() / "declared";
    fs::create_directories(folder);

    std::vector<std::string> prompted;
    PluginAuditManager::set_permission_message_prompt_for_testing(
        [&prompted](const std::string& message) {
            prompted.push_back(message);
            return true;
        });

    // "<dir>/*" asks for the folder and every folder below it. The prompt names, and the sidecar
    // stores, the canonical absolute scope the grant actually covers.
    PluginPermissions tree_create;
    tree_create.fs_create = {(folder / "*").string()};
    CHECK(mgr.request_permissions(PLUGIN, tree_create));
    REQUIRE(prompted.size() == 1);
    const std::vector<std::string> saved_create = harness.saved_permissions(PLUGIN).fs_create;
    REQUIRE(saved_create.size() == 1);
    const fs::path saved_create_path(saved_create.front());
    CHECK(saved_create_path.filename() == "*");
    CHECK(fs::equivalent(saved_create_path.parent_path(), folder));
    CHECK(prompted.front().find(saved_create.front()) != std::string::npos);

    // The saved tree grant also covers a later literal request for the same folder.
    PluginPermissions literal_create;
    literal_create.fs_create = {folder.string()};
    CHECK(mgr.request_permissions(PLUGIN, literal_create));
    CHECK(prompted.size() == 1);
    CHECK(harness.saved_permissions(PLUGIN).fs_create == saved_create);

    // Delete is an independent bucket with the same subtree syntax.
    PluginPermissions tree_delete;
    tree_delete.fs_delete = {(folder / "*").string()};
    CHECK(mgr.request_permissions(PLUGIN, tree_delete));
    REQUIRE(prompted.size() == 2);
    const std::vector<std::string> saved_delete = harness.saved_permissions(PLUGIN).fs_delete;
    REQUIRE(saved_delete.size() == 1);
    CHECK(fs::path(saved_delete.front()).filename() == "*");
    CHECK(fs::equivalent(fs::path(saved_delete.front()).parent_path(), folder));
    CHECK(harness.saved_permissions(PLUGIN).fs_create == saved_create);

    // A saved tree grant already covers a nested tree request, so it is neither prompted nor
    // stored again.
    const fs::path nested = folder / "nested";
    fs::create_directories(nested);
    PluginPermissions nested_create;
    nested_create.fs_create = {(nested / "*").string()};
    CHECK(mgr.request_permissions(PLUGIN, nested_create));
    CHECK(prompted.size() == 2);
    CHECK(harness.saved_permissions(PLUGIN).fs_create == saved_create);

    // A wildcard that is not the final tree marker does not name a scope the prompt could show.
    PluginPermissions pattern_delete;
    pattern_delete.fs_delete = {(folder / "*.txt").string()};
    CHECK_FALSE(mgr.request_permissions(PLUGIN, pattern_delete));
    CHECK(prompted.size() == 2);
    CHECK(harness.saved_permissions(PLUGIN).fs_delete == saved_delete);

    // The all-folders marker is only valid on its own.
    PluginPermissions mixed;
    mixed.fs_delete = {"*", folder.string()};
    CHECK_FALSE(mgr.request_permissions(PLUGIN, mixed));
    CHECK(prompted.size() == 2);
    CHECK(harness.saved_permissions(PLUGIN).fs_delete == saved_delete);

    // Positive control: the one-element all-folders form is accepted and stored alone.
    PluginPermissions full;
    full.fs_delete = {"*"};
    CHECK(mgr.request_permissions(PLUGIN, full));
    const std::vector<std::string> saved_full = harness.saved_permissions(PLUGIN).fs_delete;
    REQUIRE(saved_full.size() == 1);
    CHECK(saved_full.front() == "*");
    CHECK(prompted.size() == 3);

    PluginAuditManager::set_permission_message_prompt_for_testing(nullptr);
}

#if !defined(_WIN32)
TEST_CASE("A tree declaration rooted at / discloses the subtree scope, not full access", "[audit]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    PluginAuditManager& mgr = PluginAuditManager::instance();

    std::string shown;
    PluginAuditManager::set_permission_message_prompt_for_testing(
        [&shown](const std::string& message) {
            shown = message;
            return true;
        });

    // "/*" is the tree grant rooted at "/". It is a subtree scope, not the "*" all-folders marker.
    PluginPermissions requested;
    requested.fs_create = {"/*"};
    CHECK(mgr.request_permissions(PLUGIN, requested));
    CHECK(harness.saved_permissions(PLUGIN).fs_create == std::vector<std::string>{"/*"});

    // The prompt keeps the root and discloses its filesystem-wide scope.
    CHECK(shown.find("/*") != std::string::npos);
    CHECK(shown.find("entire filesystem, drive, or network share") != std::string::npos);
    CHECK(shown.find("anywhere on this computer") == std::string::npos);

    PluginAuditManager::set_permission_message_prompt_for_testing(nullptr);
}
#endif

TEST_CASE("A threading permission request is prompted once and saved as a boolean", "[audit]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    PluginAuditManager& mgr = PluginAuditManager::instance();

    PluginPermissions requested;
    requested.threading = true;

    std::string shown;
    PluginAuditManager::set_permission_message_prompt_for_testing(
        [&shown](const std::string& message) { shown = message; return true; });
    const bool granted = mgr.request_permissions(PLUGIN, requested);
    PluginAuditManager::set_permission_message_prompt_for_testing(nullptr);

    REQUIRE(granted);
    CHECK(shown.find("create a thread") != std::string::npos);
    CHECK(harness.saved_permissions(PLUGIN).threading);

    // The saved boolean grant covers a repeated declaration without asking again.
    bool prompted_again = false;
    PluginAuditManager::set_permission_message_prompt_for_testing(
        [&prompted_again](const std::string&) {
            prompted_again = true;
            return false;
        });
    CHECK(mgr.request_permissions(PLUGIN, requested));
    PluginAuditManager::set_permission_message_prompt_for_testing(nullptr);
    CHECK_FALSE(prompted_again);
}

TEST_CASE("A permission request creates a sidecar for a discovered plugin that has none", "[audit]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);

    // A sandbox plugin may be discovered without ever having been given a sidecar. An approval
    // must still be saved, so the next session does not ask again.
    const fs::path sidecar = harness.data.plugins_dir() / PLUGIN / INSTALL_STATE_FILE;
    boost::system::error_code ec;
    fs::remove(sidecar, ec);
    REQUIRE_FALSE(ec);

    PluginPermissions requested;
    requested.fs_read = {"/newly/granted"};

    PluginAuditManager::set_permission_message_prompt_for_testing(
        [](const std::string&) { return true; });
    const bool granted = PluginAuditManager::instance().request_permissions(PLUGIN, requested);
    PluginAuditManager::set_permission_message_prompt_for_testing(nullptr);

    REQUIRE(granted);
    CHECK(fs::exists(sidecar));
    CHECK(harness.saved_permissions(PLUGIN).fs_read == std::vector<std::string>{"/newly/granted"});
}

TEST_CASE("A permission request does not recreate a sidecar that disappears while prompting", "[audit]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);

    const fs::path sidecar = harness.data.plugins_dir() / PLUGIN / INSTALL_STATE_FILE;
    REQUIRE(fs::exists(sidecar));

    bool removed = false;
    PluginAuditManager::set_permission_message_prompt_for_testing(
        [&sidecar, &removed](const std::string&) {
            boost::system::error_code ec;
            removed = fs::remove(sidecar, ec);
            return true;
        });

    PluginPermissions requested;
    requested.fs_read = {"/new/grant"};
    const bool granted = PluginAuditManager::instance().request_permissions(PLUGIN, requested);
    PluginAuditManager::set_permission_message_prompt_for_testing(nullptr);

    // Readable at the first read, absent at the second: that is lost state, not a new install, so
    // the approval must not create a fresh sidecar.
    REQUIRE(removed);
    CHECK_FALSE(granted);
    CHECK_FALSE(fs::exists(sidecar));
}

TEST_CASE("A permission request leaves an unreadable install-state sidecar untouched", "[audit]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);

    const fs::path    sidecar   = harness.data.plugins_dir() / PLUGIN / INSTALL_STATE_FILE;
    const std::string malformed = "{ \"permissions\": {";
    {
        std::ofstream out(sidecar.string(), std::ios::binary | std::ios::trunc);
        out << malformed;
    }

    bool prompted = false;
    PluginAuditManager::set_permission_message_prompt_for_testing(
        [&prompted](const std::string&) {
            prompted = true;
            return true;
        });

    PluginPermissions requested;
    requested.fs_read = {"/new/grant"};
    const bool granted = PluginAuditManager::instance().request_permissions(PLUGIN, requested);
    PluginAuditManager::set_permission_message_prompt_for_testing(nullptr);

    // The file cannot be read, so no grant can be saved safely: fail closed instead of replacing
    // whatever it holds.
    CHECK_FALSE(granted);
    CHECK_FALSE(prompted);
    CHECK(read_text_file(sidecar) == malformed);
}

TEST_CASE("An approved audited operation creates a missing install-state sidecar", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);
    harness.approve = [](const Prompt&) { return true; };

    const fs::path sidecar = harness.data.plugins_dir() / PLUGIN / INSTALL_STATE_FILE;
    boost::system::error_code ec;
    fs::remove(sidecar, ec);
    REQUIRE_FALSE(ec);

    // An event grant seeds a sidecar when none exists.
    python.run_as(PLUGIN, "read_outcome = attempt(lambda: open(PRIVATE).read())");
    CHECK(python.str("read_outcome") == "ok");
    REQUIRE(fs::exists(sidecar));
    CHECK_FALSE(harness.saved_permissions(PLUGIN).fs_read.empty());

    // The threading helper does the same when the sidecar disappears again.
    fs::remove(sidecar, ec);
    REQUIRE_FALSE(ec);
    python.run_as(PLUGIN, "thread_outcome = attempt(start_and_join_thread)");
    CHECK(python.str("thread_outcome") == "ok");
    CHECK(fs::exists(sidecar));
    CHECK(harness.saved_permissions(PLUGIN).threading);
}

TEST_CASE("An approved audited operation does not replace an unreadable install-state sidecar", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);
    harness.approve = [](const Prompt&) { return true; };

    // Both the event-grant path (persist_permission) and the boolean threading path
    // (persist_threading_grant) must preserve a sidecar they cannot read.
    const fs::path    sidecar   = harness.data.plugins_dir() / PLUGIN / INSTALL_STATE_FILE;
    const std::string malformed = "not an install state";
    {
        std::ofstream out(sidecar.string(), std::ios::binary | std::ios::trunc);
        out << malformed;
    }

    // The operation itself is approved for this call; only the persistence is refused.
    python.run_as(PLUGIN, "read_outcome = attempt(lambda: open(PRIVATE).read())");
    python.run_as(PLUGIN, "thread_outcome = attempt(start_and_join_thread)");

    CHECK(python.str("read_outcome") == "ok");
    CHECK(python.str("thread_outcome") == "ok");
    CHECK(read_text_file(sidecar) == malformed);
}

TEST_CASE("Thread creation is denied without a grant and remembered after approval", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    // The audit hook's thread event has no user-supplied target; the fixed sentinel is what the
    // prompt and the persisted grant use. The helper joins the worker so no test thread outlives
    // the test case.
    python.run_as(PLUGIN, "thread_outcome = attempt(start_and_join_thread)");
    CHECK(python.str("thread_outcome") == "denied");
    CHECK(describe(harness.prompts) == "_thread.start_new_thread: thread\n");
    CHECK_FALSE(harness.saved_permissions(PLUGIN).threading);

    harness.approve = [](const Prompt&) { return true; };
    harness.prompts.clear();
    python.run_as(PLUGIN, "thread_outcome = attempt(start_and_join_thread)");
    CHECK(python.str("thread_outcome") == "ok");
    CHECK(describe(harness.prompts) == "_thread.start_new_thread: thread\n");
    CHECK(harness.saved_permissions(PLUGIN).threading);

    // The saved grant is effective for later events without another prompt.
    harness.prompts.clear();
    python.run_as(PLUGIN, "thread_outcome = attempt(start_and_join_thread)");
    CHECK(python.str("thread_outcome") == "ok");
    CHECK(describe(harness.prompts).empty());
}

TEST_CASE("Full access allows an audited operation without a grant", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    PluginPermissions permissions;
    permissions.full_access = true;
    harness.save_permissions(PLUGIN, permissions);

    // PRIVATE sits outside every allowed root, so without full access this read would prompt.
    python.run_as(PLUGIN, "outcome = attempt(lambda: open(PRIVATE).read())");

    CHECK(python.str("outcome") == "ok");
    CHECK(describe(harness.prompts).empty());
}

TEST_CASE("Without full access an audited operation still prompts", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    PluginPermissions permissions;
    permissions.full_access = false;
    harness.save_permissions(PLUGIN, permissions);

    python.run_as(PLUGIN, "outcome = attempt(lambda: open(PRIVATE).read())");

    CHECK(python.str("outcome") == "denied");
    CHECK(describe(harness.prompts) == "open: " + python.str("PRIVATE") + "\n");
}

TEST_CASE("Full access does not bypass an unconditional denied path", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    seed_denied_names();
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    PluginPermissions permissions;
    permissions.full_access = true;
    harness.save_permissions(PLUGIN, permissions);

    // The install-state sidecar stays host-managed: full access only switches off permission
    // auditing, it does not make the plugin's own state file reachable.
    python.globals["INSTALL_STATE"] = (harness.data.plugins_dir() / PLUGIN / ".install_state.json").string();
    python.run_as(PLUGIN, "denied_outcome = attempt(lambda: open(INSTALL_STATE).read())");

    CHECK(python.str("denied_outcome") == "denied");
    CHECK(describe(harness.prompts).empty());
    AuditViolation violation;
    REQUIRE(PluginAuditManager::instance().last_violation(violation));
    CHECK(violation.reason == "denied path");
}

TEST_CASE("A full access plugin's permission declaration is not prompted", "[audit]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);

    PluginPermissions stored;
    stored.full_access = true;
    stored.fs_read     = {"/previously/granted"};
    harness.save_permissions(PLUGIN, stored);

    bool prompted = false;
    PluginAuditManager::set_permission_message_prompt_for_testing(
        [&prompted](const std::string&) {
            prompted = true;
            return false;
        });

    PluginPermissions requested;
    requested.fs_read = {"/new/target"};
    const bool granted = PluginAuditManager::instance().request_permissions(PLUGIN, requested);

    PluginAuditManager::set_permission_message_prompt_for_testing(nullptr);

    // The declaration is accepted without a prompt and without adding a grant: full access already
    // covers the event, and the stored list stays as it was.
    CHECK(granted);
    CHECK_FALSE(prompted);
    CHECK(harness.saved_permissions(PLUGIN).fs_read == std::vector<std::string>{"/previously/granted"});
}

TEST_CASE("Toggling full access preserves the other stored grants", "[audit]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);

    PluginPermissions stored;
    stored.fs_read        = {"/kept/read"};
    stored.network_http   = {"https://kept.example"};
    harness.save_permissions(PLUGIN, stored);

    PluginManager& manager = PluginManager::instance();
    PluginPermissionMutation toggle;
    toggle.full_access = true;
    REQUIRE(manager.mutate_plugin_permissions(PLUGIN, toggle));
    PluginPermissions after_enable = harness.saved_permissions(PLUGIN);
    CHECK(after_enable.full_access);
    CHECK(after_enable.fs_read == std::vector<std::string>{"/kept/read"});
    CHECK(after_enable.network_http == std::vector<std::string>{"https://kept.example"});

    toggle.full_access = false;
    REQUIRE(manager.mutate_plugin_permissions(PLUGIN, toggle));
    PluginPermissions after_disable = harness.saved_permissions(PLUGIN);
    CHECK_FALSE(after_disable.full_access);
    CHECK(after_disable.fs_read == std::vector<std::string>{"/kept/read"});

    // An unknown package has no sidecar to modify: the toggle fails closed.
    toggle.full_access = true;
    CHECK_FALSE(manager.mutate_plugin_permissions("NoSuchPlugin", toggle));
}

TEST_CASE("A permission mutation changes only the named entries and preserves the rest", "[audit]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);

    const fs::path plugin_dir = harness.data.plugins_dir() / PLUGIN;
    PluginInstallState initial;
    initial.installed_from    = "local";
    initial.installed_version = "1.2.3";
    initial.plugin_name       = "Audit Plugin";
    initial.enabled           = false;
    initial.capabilities      = {{"Echo", false}, {"Other", true}};
    initial.permissions.fs_read      = {"/kept/read", "/removed/read"};
    initial.permissions.fs_readwrite = {"/kept/write"};
    initial.permissions.fs_create    = {"/kept/create", "/removed/create"};
    initial.permissions.fs_delete    = {"/kept/delete", "/removed/delete"};
    initial.permissions.network_http = {"https://kept.example"};
    REQUIRE(write_install_state(plugin_dir, initial));

    PluginPermissionMutation mutation;
    mutation.add = {
        {PluginPermissionCategory::FsRead, "/added/read"},
        {PluginPermissionCategory::FsRead, "/added/read"}, // duplicate is collapsed
        {PluginPermissionCategory::FsRead, ""},            // empty values are skipped
        {PluginPermissionCategory::FsRead, "/removed/read"},
        {PluginPermissionCategory::FsCreate, "/added/create"},
        {PluginPermissionCategory::FsCreate, "/removed/create"},
        {PluginPermissionCategory::FsDelete, "/added/delete"},
        {PluginPermissionCategory::NetworkSocket, "127.0.0.1:8080"},
    };
    mutation.remove = {
        {PluginPermissionCategory::FsRead, "/removed/read"},
        {PluginPermissionCategory::FsRead, "/never/granted"},
        {PluginPermissionCategory::FsRead, ""},
        {PluginPermissionCategory::FsCreate, "/removed/create"},
        {PluginPermissionCategory::FsDelete, "/removed/delete"},
        {PluginPermissionCategory::Process, "/never/granted"},
    };

    PluginPermissions applied;
    REQUIRE(PluginManager::instance().mutate_plugin_permissions(PLUGIN, mutation, &applied));
    // Removals run first, so a value named in both lists ends present; absent removals are ignored.
    const std::vector<std::string> expected_fs_read{"/kept/read", "/added/read", "/removed/read"};
    const std::vector<std::string> expected_fs_create{"/kept/create", "/added/create", "/removed/create"};
    const std::vector<std::string> expected_fs_delete{"/kept/delete", "/added/delete"};
    const std::vector<std::string> expected_sockets{"127.0.0.1:8080"};
    CHECK(applied.fs_read == expected_fs_read);
    CHECK(applied.fs_create == expected_fs_create);
    CHECK(applied.fs_delete == expected_fs_delete);
    CHECK(applied.network_socket == expected_sockets);

    PluginInstallState stored;
    REQUIRE(read_install_state(plugin_dir, stored));
    CHECK(stored.permissions.fs_read == expected_fs_read);
    CHECK(stored.permissions.fs_create == expected_fs_create);
    CHECK(stored.permissions.fs_delete == expected_fs_delete);
    CHECK(stored.permissions.network_socket == expected_sockets);

    // Every grant the mutation did not name stays as it was.
    CHECK(stored.permissions.fs_readwrite == initial.permissions.fs_readwrite);
    CHECK(stored.permissions.network_http == initial.permissions.network_http);
    CHECK(stored.permissions.process.empty());
    CHECK_FALSE(stored.permissions.threading);
    CHECK_FALSE(stored.permissions.full_access);

    // The install metadata, enabled flag and capability flags are unrelated state and survive too.
    CHECK(stored.installed_from == initial.installed_from);
    CHECK(stored.installed_version == initial.installed_version);
    CHECK(stored.plugin_name == initial.plugin_name);
    CHECK_FALSE(stored.enabled);
    REQUIRE(stored.capabilities.size() == 2);
    CHECK(stored.capabilities[0].first == "Echo");
    CHECK_FALSE(stored.capabilities[0].second);
    CHECK(stored.capabilities[1].first == "Other");
    CHECK(stored.capabilities[1].second);
}

TEST_CASE("A permission mutation applies to the latest stored contents", "[audit]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);

    PluginPermissions initial;
    initial.fs_read = {"/caller/saw"};
    harness.save_permissions(PLUGIN, initial);

    PluginManager& manager = PluginManager::instance();
    PluginInstallState observed;
    REQUIRE(manager.get_install_state(PLUGIN, observed));
    CHECK(observed.permissions.fs_read == std::vector<std::string>{"/caller/saw"});

    // Another writer grants a target after the caller read the state.
    PluginPermissionMutation concurrent;
    concurrent.add = {{PluginPermissionCategory::FsRead, "/other/writer"}};
    REQUIRE(manager.mutate_plugin_permissions(PLUGIN, concurrent));

    // The caller's edit is applied against the sidecar as it stands now, not against `observed`,
    // so the concurrent grant is not dropped.
    PluginPermissionMutation mine;
    mine.add = {{PluginPermissionCategory::FsRead, "/caller/adds"}};
    REQUIRE(manager.mutate_plugin_permissions(PLUGIN, mine));

    PluginInstallState stored;
    REQUIRE(read_install_state(harness.data.plugins_dir() / PLUGIN, stored));
    const std::vector<std::string> expected_fs_read{"/caller/saw", "/other/writer", "/caller/adds"};
    CHECK(stored.permissions.fs_read == expected_fs_read);
}

TEST_CASE("A permission mutation changes boolean grants only when asked", "[audit]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);

    PluginPermissions initial;
    initial.fs_read     = {"/kept"};
    initial.full_access = true;
    harness.save_permissions(PLUGIN, initial);

    PluginManager& manager = PluginManager::instance();

    // A mutation that names no booleans leaves both flags alone.
    PluginPermissions applied;
    REQUIRE(manager.mutate_plugin_permissions(PLUGIN, PluginPermissionMutation{}, &applied));
    CHECK_FALSE(applied.threading);
    CHECK(applied.full_access);
    CHECK(applied.fs_read == std::vector<std::string>{"/kept"});

    PluginPermissionMutation booleans;
    booleans.threading   = true;
    booleans.full_access = false;
    REQUIRE(manager.mutate_plugin_permissions(PLUGIN, booleans, &applied));
    CHECK(applied.threading);
    CHECK_FALSE(applied.full_access);
    CHECK(applied.fs_read == std::vector<std::string>{"/kept"});

    const PluginPermissions stored = harness.saved_permissions(PLUGIN);
    CHECK(stored.threading);
    CHECK_FALSE(stored.full_access);
    CHECK(stored.fs_read == std::vector<std::string>{"/kept"});
}

TEST_CASE("A permission mutation fails closed for a plugin it cannot update", "[audit]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);

    PluginPermissionMutation mutation;
    mutation.add = {{PluginPermissionCategory::FsRead, "/new"}};
    PluginManager& manager = PluginManager::instance();

    CHECK_FALSE(manager.mutate_plugin_permissions("", mutation));
    CHECK_FALSE(manager.mutate_plugin_permissions("NoSuchPlugin", mutation));

    // A discovered package whose sidecar is gone has nothing safe to update.
    const fs::path sidecar = harness.data.plugins_dir() / PLUGIN / INSTALL_STATE_FILE;
    fs::remove(sidecar);
    CHECK_FALSE(manager.mutate_plugin_permissions(PLUGIN, mutation));
}

TEST_CASE("A permission edit applies to the next audited operation without a reload", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    python.run_as(PLUGIN, "first_outcome = attempt(lambda: open(PRIVATE).read())");
    CHECK(python.str("first_outcome") == "denied");
    harness.prompts.clear();

    PluginPermissionMutation mutation;
    mutation.full_access = true;
    REQUIRE(PluginManager::instance().mutate_plugin_permissions(PLUGIN, mutation));

    // The running audit re-reads the sidecar, so the edit covers the next operation.
    python.run_as(PLUGIN, "second_outcome = attempt(lambda: open(PRIVATE).read())");
    CHECK(python.str("second_outcome") == "ok");
    CHECK(describe(harness.prompts).empty());
}

TEST_CASE("A filesystem wildcard keeps read and write grants separate", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    python.exec(R"PY(
NESTED_FILE = os.path.join(TREE, "one", "f.txt")
NEW_FILE = os.path.join(TREE, "one", "new.txt")
)PY");
    const std::string tree = python.str("TREE");
    PluginPermissions permissions;
    permissions.fs_read = {tree + "/*"};
    harness.save_permissions(PLUGIN, permissions);

    python.run_as(PLUGIN, R"PY(
read_outcome = attempt(lambda: open(NESTED_FILE).read())
write_outcome = attempt(lambda: open(NEW_FILE, "w").close())
)PY");
    CHECK(python.str("read_outcome") == "ok");
    CHECK(python.str("write_outcome") == "denied");
    // A read grant does not cover the write-shaped open, so that one is asked about.
    const std::string write_prompt = "open: " + python.str("NEW_FILE") + "\n";
    CHECK(describe(harness.prompts) == write_prompt);

    // The reverse direction is separate too: a read/write wildcard leaves plain reads in the
    // read bucket, which is now empty.
    permissions.fs_read.clear();
    permissions.fs_readwrite = {tree + "/*"};
    harness.save_permissions(PLUGIN, permissions);
    harness.prompts.clear();
    python.run_as(PLUGIN, R"PY(
write_outcome = attempt(lambda: open(NEW_FILE, "w").close())
reread_outcome = attempt(lambda: open(NESTED_FILE).read())
)PY");
    CHECK(python.str("write_outcome") == "ok");
    CHECK(python.str("reread_outcome") == "denied");
    const std::string reread_prompt = "open: " + python.str("NESTED_FILE") + "\n";
    CHECK(describe(harness.prompts) == reread_prompt);
}

TEST_CASE("Create and delete folder grants cover direct entries but not subfolders", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    python.exec(R"PY(
GRANTED = os.path.realpath(os.path.join(OUTSIDE, "granted"))
SUBFOLDER = os.path.join(GRANTED, "sub")
os.makedirs(SUBFOLDER)
DELETE_FILE = os.path.join(GRANTED, "delete-me.txt")
with open(DELETE_FILE, "w") as f:
    f.write("x")
NESTED_DELETE_FILE = os.path.join(SUBFOLDER, "delete-me-too.txt")
with open(NESTED_DELETE_FILE, "w") as f:
    f.write("x")
NEW_DIR = os.path.join(GRANTED, "new-dir")
OTHER_DIR = os.path.join(GRANTED, "other-dir")
NESTED_NEW_DIR = os.path.join(SUBFOLDER, "new-dir")
)PY");

    PluginPermissions permissions;
    permissions.fs_create = {python.str("GRANTED")};
    permissions.fs_delete = {python.str("GRANTED")};
    harness.save_permissions(PLUGIN, permissions);

    python.run_as(PLUGIN, R"PY(
create_outcome = attempt(lambda: os.mkdir(NEW_DIR))
delete_outcome = attempt(lambda: os.remove(DELETE_FILE))
nested_create_outcome = attempt(lambda: os.mkdir(NESTED_NEW_DIR))
nested_delete_outcome = attempt(lambda: os.remove(NESTED_DELETE_FILE))
)PY");

    // The literal grant names the exact folder, so an operation directly inside it is covered; an
    // operation in a child folder is not. The tree form below covers both.
    CHECK(python.str("create_outcome") == "ok");
    CHECK(python.str("delete_outcome") == "ok");
    CHECK(python.str("nested_create_outcome") == "denied");
    CHECK(python.str("nested_delete_outcome") == "denied");
    CHECK(describe(harness.prompts) == "os.mkdir: " + python.str("SUBFOLDER") + "\n" +
                                            "os.remove: " + python.str("SUBFOLDER") + "\n");
    CHECK(fs::is_directory(fs::path(python.str("NEW_DIR"))));
    CHECK(fs::exists(fs::path(python.str("NESTED_DELETE_FILE"))));
    CHECK_FALSE(fs::exists(fs::path(python.str("NESTED_NEW_DIR"))));

    // Create and delete are independent buckets: a delete grant does not authorize a create in
    // the same folder, and the reverse.
    permissions.fs_create.clear();
    permissions.fs_delete = {python.str("GRANTED")};
    harness.save_permissions(PLUGIN, permissions);
    harness.prompts.clear();
    python.run_as(PLUGIN, R"PY(
other_create_outcome = attempt(lambda: os.mkdir(OTHER_DIR))
other_delete_outcome = attempt(lambda: os.rmdir(NEW_DIR))
)PY");

    CHECK(python.str("other_create_outcome") == "denied");
    CHECK(python.str("other_delete_outcome") == "ok");
    CHECK(describe(harness.prompts) == "os.mkdir: " + python.str("GRANTED") + "\n");
}

TEST_CASE("A create and delete tree grant covers direct entries and subfolders but not a sibling", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    python.exec(R"PY(
GRANTED = os.path.realpath(os.path.join(OUTSIDE, "tree-granted"))
SIBLING = os.path.realpath(os.path.join(OUTSIDE, "tree-granted-sibling"))
SUBFOLDER = os.path.join(GRANTED, "sub")
os.makedirs(SUBFOLDER)
os.makedirs(SIBLING)
TREE_GRANT = GRANTED + os.sep + "*"
DIRECT_FILE = os.path.join(GRANTED, "direct.txt")
NESTED_FILE = os.path.join(SUBFOLDER, "nested.txt")
SIBLING_FILE = os.path.join(SIBLING, "sibling.txt")
for path in (DIRECT_FILE, NESTED_FILE, SIBLING_FILE):
    with open(path, "w") as f:
        f.write("x")
DIRECT_NEW = os.path.join(GRANTED, "direct-new")
NESTED_NEW = os.path.join(SUBFOLDER, "nested-new")
SIBLING_NEW = os.path.join(SIBLING, "sibling-new")
)PY");

    // "<dir>/*" is a subtree grant, unlike the literal folder grant in the previous test.
    PluginPermissions permissions;
    permissions.fs_create = {python.str("TREE_GRANT")};
    permissions.fs_delete = {python.str("TREE_GRANT")};
    harness.save_permissions(PLUGIN, permissions);

    python.run_as(PLUGIN, R"PY(
direct_create_outcome = attempt(lambda: os.mkdir(DIRECT_NEW))
nested_create_outcome = attempt(lambda: os.mkdir(NESTED_NEW))
direct_delete_outcome = attempt(lambda: os.remove(DIRECT_FILE))
nested_delete_outcome = attempt(lambda: os.remove(NESTED_FILE))
sibling_create_outcome = attempt(lambda: os.mkdir(SIBLING_NEW))
sibling_delete_outcome = attempt(lambda: os.remove(SIBLING_FILE))
)PY");

    CHECK(python.str("direct_create_outcome") == "ok");
    CHECK(python.str("nested_create_outcome") == "ok");
    CHECK(python.str("direct_delete_outcome") == "ok");
    CHECK(python.str("nested_delete_outcome") == "ok");
    // A sibling whose name only shares the granted folder's text prefix is a different folder.
    CHECK(python.str("sibling_create_outcome") == "denied");
    CHECK(python.str("sibling_delete_outcome") == "denied");
    CHECK(describe(harness.prompts) == "os.mkdir: " + python.str("SIBLING") + "\n" +
                                            "os.remove: " + python.str("SIBLING") + "\n");
    CHECK(fs::is_directory(fs::path(python.str("DIRECT_NEW"))));
    CHECK(fs::is_directory(fs::path(python.str("NESTED_NEW"))));
    CHECK_FALSE(fs::exists(fs::path(python.str("DIRECT_FILE"))));
    CHECK_FALSE(fs::exists(fs::path(python.str("NESTED_FILE"))));
    CHECK_FALSE(fs::exists(fs::path(python.str("SIBLING_NEW"))));
    CHECK(fs::exists(fs::path(python.str("SIBLING_FILE"))));

    // Create and delete stay separate: dropping the create grant denies a later create in the
    // same subtree while the delete grant keeps working.
    permissions.fs_create.clear();
    harness.save_permissions(PLUGIN, permissions);
    harness.prompts.clear();
    python.run_as(PLUGIN, R"PY(
cross_create_outcome = attempt(lambda: os.mkdir(os.path.join(GRANTED, "cross-new")))
cross_delete_outcome = attempt(lambda: os.rmdir(NESTED_NEW))
)PY");
    CHECK(python.str("cross_create_outcome") == "denied");
    CHECK(python.str("cross_delete_outcome") == "ok");
    CHECK(describe(harness.prompts) == "os.mkdir: " + python.str("GRANTED") + "\n");
}

TEST_CASE("A create and delete tree grant does not follow a symlink out of its root", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    python.exec(R"PY(
GRANTED = os.path.realpath(os.path.join(OUTSIDE, "tree-link-granted"))
ESCAPE_TARGET = os.path.realpath(os.path.join(OUTSIDE, "tree-link-target"))
os.makedirs(GRANTED)
os.makedirs(ESCAPE_TARGET)
TREE_GRANT = GRANTED + os.sep + "*"
VICTIM = os.path.join(ESCAPE_TARGET, "victim.txt")
with open(VICTIM, "w") as f:
    f.write("x")
LINK = os.path.join(GRANTED, "escape")
try:
    os.symlink(ESCAPE_TARGET, LINK, target_is_directory=True)
    SYMLINK_OK = True
except (OSError, NotImplementedError):
    SYMLINK_OK = False
)PY");
    if (python.eval("SYMLINK_OK") != "True")
        SKIP("Cannot create a directory symlink here");

    PluginPermissions permissions;
    permissions.fs_create = {python.str("TREE_GRANT")};
    permissions.fs_delete = {python.str("TREE_GRANT")};
    harness.save_permissions(PLUGIN, permissions);

    // The link is inside the granted tree, but the folder it resolves to is outside it. Creating
    // or deleting through the link must be judged against that outside folder.
    python.run_as(PLUGIN, R"PY(
escaped_create_outcome = attempt(lambda: os.mkdir(os.path.join(LINK, "new")))
escaped_delete_outcome = attempt(lambda: os.remove(os.path.join(LINK, "victim.txt")))
)PY");

    CHECK(python.str("escaped_create_outcome") == "denied");
    CHECK(python.str("escaped_delete_outcome") == "denied");
    CHECK(describe(harness.prompts) == "os.mkdir: " + python.str("ESCAPE_TARGET") + "\n" +
                                            "os.remove: " + python.str("ESCAPE_TARGET") + "\n");
    CHECK(fs::exists(fs::path(python.str("VICTIM"))));
    CHECK_FALSE(fs::exists(fs::path(python.str("ESCAPE_TARGET")) / "new"));

    // Removing the link itself acts on the link inside the granted folder, not on its target.
    harness.prompts.clear();
    python.run_as(PLUGIN, "link_delete_outcome = attempt(lambda: os.remove(LINK))");
    CHECK(python.str("link_delete_outcome") == "ok");
    CHECK(describe(harness.prompts).empty());
    CHECK_FALSE(fs::is_symlink(fs::path(python.str("LINK"))));
    CHECK(fs::exists(fs::path(python.str("VICTIM"))));
}

#if !defined(_WIN32)
TEST_CASE("A create or delete target inside a folder literally named * fails closed without a tree grant", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    python.exec(R"PY(
BASE = os.path.realpath(os.path.join(OUTSIDE, "star-parent"))
STAR = os.path.join(BASE, "*")
os.makedirs(STAR)
os.makedirs(os.path.join(BASE, "sibling"))
STAR_FILE = os.path.join(STAR, "kept.txt")
with open(STAR_FILE, "w") as f:
    f.write("x")
STAR_NEW = os.path.join(STAR, "new")
SIBLING_NEW = os.path.join(BASE, "sibling", "new")
)PY");

    // The parent folder is spelled "/.../*". Storing it as-is would be read back as a tree grant
    // for the whole parent, so an approval here must fail closed instead.
    harness.approve = [](const Prompt&) { return true; };
    python.run_as(PLUGIN, "star_create_outcome = attempt(lambda: os.mkdir(STAR_NEW))");

    CHECK(python.str("star_create_outcome") == "denied");
    CHECK_FALSE(fs::exists(fs::path(python.str("STAR_NEW"))));
    CHECK(harness.saved_permissions(PLUGIN).fs_create.empty());

    // Nothing was widened to the parent: an operation beside the literal "*" folder is still
    // prompted and denied, and no tree grant for BASE was stored.
    harness.prompts.clear();
    harness.approve = [](const Prompt&) { return false; };
    python.run_as(PLUGIN, "sibling_outcome = attempt(lambda: os.mkdir(SIBLING_NEW))");

    CHECK(python.str("sibling_outcome") == "denied");
    CHECK_FALSE(fs::exists(fs::path(python.str("SIBLING_NEW"))));
    CHECK(harness.saved_permissions(PLUGIN).fs_create.empty());

    // Delete resolves the same parent folder and must not persist a tree grant either.
    harness.prompts.clear();
    harness.approve = [](const Prompt&) { return true; };
    python.run_as(PLUGIN, "star_delete_outcome = attempt(lambda: os.remove(STAR_FILE))");

    CHECK(python.str("star_delete_outcome") == "denied");
    CHECK(fs::exists(fs::path(python.str("STAR_FILE"))));
    CHECK(harness.saved_permissions(PLUGIN).fs_delete.empty());
}
#endif

TEST_CASE("A full create and delete grant allows any folder but not a denied path", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    python.exec(R"PY(
FIRST = os.path.join(OUTSIDE, "full-create")
os.makedirs(FIRST)
NESTED = os.path.join(TREE, "one", "full-create")
DELETE_FILES = [os.path.join(FIRST, "a.txt"), os.path.join(TREE, "two", "b.txt")]
for path in DELETE_FILES:
    with open(path, "w") as f:
        f.write("x")
DENIED_DIR = os.path.join(OUTSIDE, "secrets")
os.makedirs(DENIED_DIR)
DENIED_FILE = os.path.join(DENIED_DIR, "token.txt")
with open(DENIED_FILE, "w") as f:
    f.write("x")
DENIED_NEW = os.path.join(DENIED_DIR, "new")
)PY");

    // "*" is the stored form of "all folders", while a "<dir>/*" entry grants one subtree.
    PluginPermissions permissions;
    permissions.fs_create = {"*"};
    permissions.fs_delete = {"*"};
    harness.save_permissions(PLUGIN, permissions);

    python.run_as(PLUGIN, R"PY(
create_outcome = attempt(lambda: os.mkdir(NESTED))
delete_outcome = attempt(lambda: [os.remove(path) for path in DELETE_FILES])
denied_create_outcome = attempt(lambda: os.mkdir(DENIED_NEW))
denied_delete_outcome = attempt(lambda: os.remove(DENIED_FILE))
)PY");

    CHECK(python.str("create_outcome") == "ok");
    CHECK(python.str("delete_outcome") == "ok");
    CHECK(python.str("denied_create_outcome") == "denied");
    CHECK(python.str("denied_delete_outcome") == "denied");
    CHECK(describe(harness.prompts).empty());
    CHECK(fs::is_directory(fs::path(python.str("NESTED"))));
    CHECK(fs::exists(fs::path(python.str("DENIED_FILE"))));
    CHECK_FALSE(fs::exists(fs::path(python.str("DENIED_NEW"))));

    AuditViolation violation;
    REQUIRE(PluginAuditManager::instance().last_violation(violation));
    CHECK(violation.reason == "denied path");
}

TEST_CASE("An approved create or delete prompt saves the parent folder and is not asked again", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    python.exec(R"PY(
CREATE_ROOT = os.path.realpath(os.path.join(OUTSIDE, "approved-create"))
os.makedirs(CREATE_ROOT)
NESTED_FOLDER = os.path.join(CREATE_ROOT, "sub")
os.makedirs(NESTED_FOLDER)
FIRST_CREATE = os.path.join(CREATE_ROOT, "first")
SECOND_CREATE = os.path.join(CREATE_ROOT, "second")
NESTED_CREATE = os.path.join(NESTED_FOLDER, "nested")
DELETE_ROOT = os.path.realpath(os.path.join(OUTSIDE, "approved-delete"))
os.makedirs(DELETE_ROOT)
FIRST_DELETE = os.path.join(DELETE_ROOT, "one.txt")
SECOND_DELETE = os.path.join(DELETE_ROOT, "two.txt")
for path in (FIRST_DELETE, SECOND_DELETE):
    with open(path, "w") as f:
        f.write("x")
CROSS_CREATE = os.path.join(DELETE_ROOT, "cross")
)PY");

    harness.approve = [](const Prompt& prompt) { return prompt.event == "os.mkdir"; };
    python.run_as(PLUGIN, "first_create_outcome = attempt(lambda: os.mkdir(FIRST_CREATE))");

    CHECK(python.str("first_create_outcome") == "ok");
    CHECK(describe(harness.prompts) == "os.mkdir: " + python.str("CREATE_ROOT") + "\n");
    REQUIRE(harness.prompts.size() == 1);
    // The prompt shows the triggering path, not only the folder the approval saves.
    CHECK(harness.prompts.front().entries ==
          std::vector<PluginAuditManager::PermissionPromptEntry>{
              {python.str("FIRST_CREATE"), python.str("CREATE_ROOT")}});
    PluginPermissions after_create = harness.saved_permissions(PLUGIN);
    // The prompt approval is saved as the folder containing the target, not the target itself.
    REQUIRE(after_create.fs_create.size() == 1);
    CHECK(fs::equivalent(fs::path(after_create.fs_create.front()), fs::path(python.str("CREATE_ROOT"))));
    CHECK(after_create.fs_delete.empty());

    // The saved parent folder covers another operation directly inside it, without a prompt.
    harness.prompts.clear();
    python.run_as(PLUGIN, "second_create_outcome = attempt(lambda: os.mkdir(SECOND_CREATE))");
    CHECK(python.str("second_create_outcome") == "ok");
    CHECK(describe(harness.prompts).empty());

    // A subfolder is a descendant, not the granted folder, so it still asks.
    harness.approve = [](const Prompt&) { return false; };
    python.run_as(PLUGIN, "nested_create_outcome = attempt(lambda: os.mkdir(NESTED_CREATE))");
    CHECK(python.str("nested_create_outcome") == "denied");
    CHECK(describe(harness.prompts) == "os.mkdir: " + python.str("NESTED_FOLDER") + "\n");
    REQUIRE(harness.prompts.size() == 1);
    CHECK(harness.prompts.front().entries ==
          std::vector<PluginAuditManager::PermissionPromptEntry>{
              {python.str("NESTED_CREATE"), python.str("NESTED_FOLDER")}});
    const PluginPermissions after_denial = harness.saved_permissions(PLUGIN);
    REQUIRE(after_denial.fs_create.size() == 1);
    CHECK(fs::equivalent(fs::path(after_denial.fs_create.front()), fs::path(python.str("CREATE_ROOT"))));

    // Delete approval persists its own folder and leaves the create grant alone.
    harness.prompts.clear();
    harness.approve = [](const Prompt& prompt) { return prompt.event == "os.remove"; };
    python.run_as(PLUGIN, "first_delete_outcome = attempt(lambda: os.remove(FIRST_DELETE))");
    CHECK(python.str("first_delete_outcome") == "ok");
    CHECK(describe(harness.prompts) == "os.remove: " + python.str("DELETE_ROOT") + "\n");
    REQUIRE(harness.prompts.size() == 1);
    CHECK(harness.prompts.front().entries ==
          std::vector<PluginAuditManager::PermissionPromptEntry>{
              {python.str("FIRST_DELETE"), python.str("DELETE_ROOT")}});
    PluginPermissions after_delete = harness.saved_permissions(PLUGIN);
    REQUIRE(after_delete.fs_delete.size() == 1);
    CHECK(fs::equivalent(fs::path(after_delete.fs_delete.front()), fs::path(python.str("DELETE_ROOT"))));
    REQUIRE(after_delete.fs_create.size() == 1);
    CHECK(fs::equivalent(fs::path(after_delete.fs_create.front()), fs::path(python.str("CREATE_ROOT"))));

    harness.prompts.clear();
    python.run_as(PLUGIN, "second_delete_outcome = attempt(lambda: os.remove(SECOND_DELETE))");
    CHECK(python.str("second_delete_outcome") == "ok");
    CHECK(describe(harness.prompts).empty());

    // The delete grant does not authorize a create in the same folder.
    harness.approve = [](const Prompt&) { return false; };
    python.run_as(PLUGIN, "cross_create_outcome = attempt(lambda: os.mkdir(CROSS_CREATE))");
    CHECK(python.str("cross_create_outcome") == "denied");
    CHECK(describe(harness.prompts) == "os.mkdir: " + python.str("DELETE_ROOT") + "\n");
    REQUIRE(harness.prompts.size() == 1);
    // A delete grant in this folder does not cover the create target, so the prompt still shows
    // the target and the folder a create grant would save.
    CHECK(harness.prompts.front().entries ==
          std::vector<PluginAuditManager::PermissionPromptEntry>{
              {python.str("CROSS_CREATE"), python.str("DELETE_ROOT")}});
}

TEST_CASE("A create prompt lists each target with its saved folder and states the exact grant scope", "[audit]")
{
    const std::string text = PluginAuditManager::format_create_delete_permission_prompt_for_testing(
        AuditEventCategory::FsCreate, "PromptPlugin",
        {{"/work/first.txt", "/work"}, {"/work/second.txt", "/work"}, {"/other/third.txt", "/other"}});

    const std::string expected =
        "Plugin \"PromptPlugin\" is requesting to create the following entries:\n"
        "Target: /work/first.txt\n"
        "Saved folder: /work\n"
        "Target: /work/second.txt\n"
        "Saved folder: /work\n"
        "Target: /other/third.txt\n"
        "Saved folder: /other\n"
        "\n"
        "If you approve, the folder shown with each target is saved as a grant for future operations. "
        "Each grant covers creating entries directly inside its folder. It does not cover the subfolders inside that folder.";
    CHECK(text == expected);
}

TEST_CASE("A delete prompt names the target and the folder saved as its exact grant", "[audit]")
{
    const std::string text = PluginAuditManager::format_create_delete_permission_prompt_for_testing(
        AuditEventCategory::FsDelete, "PromptPlugin", {{"/work/old.txt", "/work"}});

    const std::string expected =
        "Plugin \"PromptPlugin\" is requesting to delete the following entry:\n"
        "Target: /work/old.txt\n"
        "Saved folder: /work\n"
        "\n"
        "If you approve, the folder shown with this target is saved as a grant for future operations. "
        "The grant covers deleting entries directly inside that folder. It does not cover the subfolders inside that folder.";
    CHECK(text == expected);
}

TEST_CASE("A create prompt shows the target and the canonical folder the approval will grant", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    python.exec(R"PY(
REAL_FOLDER = os.path.realpath(os.path.join(OUTSIDE, "prompt-real"))
os.makedirs(REAL_FOLDER)
LINK = os.path.join(OUTSIDE, "prompt-link")
CREATED = os.path.join(REAL_FOLDER, "new")
RAW_CREATE = os.path.join(LINK, "new")
try:
    os.symlink(REAL_FOLDER, LINK, target_is_directory=True)
    SYMLINK_OK = True
except (OSError, NotImplementedError):
    SYMLINK_OK = False
)PY");
    if (python.eval("SYMLINK_OK") != "True")
        SKIP("Cannot create a directory symlink here");

    harness.approve = [](const Prompt& prompt) { return prompt.event == "os.mkdir"; };
    python.run_as(PLUGIN, "outcome = attempt(lambda: os.mkdir(RAW_CREATE))");

    CHECK(python.str("outcome") == "ok");
    // The user approved creating inside the symlink's target. Showing only the raw path would hide
    // which folder is actually stored and granted, so the prompt shows both.
    REQUIRE(harness.prompts.size() == 1);
    CHECK(harness.prompts.front().targets == std::vector<std::string>{python.str("REAL_FOLDER")});
    CHECK(harness.prompts.front().entries ==
          std::vector<PluginAuditManager::PermissionPromptEntry>{
              {python.str("RAW_CREATE"), python.str("REAL_FOLDER")}});
    CHECK(harness.saved_permissions(PLUGIN).fs_create == std::vector<std::string>{python.str("REAL_FOLDER")});
    CHECK(fs::is_directory(fs::path(python.str("CREATED"))));
}

TEST_CASE("A relative create target is resolved against the working directory and disclosed absolutely", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    python.exec(R"PY(
RELATIVE_ROOT = os.path.realpath(os.path.join(OUTSIDE, "relative-root"))
os.makedirs(RELATIVE_ROOT)
RELATIVE_VICTIM = os.path.join(RELATIVE_ROOT, "victim.txt")
RELATIVE_SECOND_VICTIM = os.path.join(RELATIVE_ROOT, "victim2.txt")
for path in (RELATIVE_VICTIM, RELATIVE_SECOND_VICTIM):
    with open(path, "w") as f:
        f.write("x")
)PY");

    harness.approve = [](const Prompt& prompt) { return prompt.event == "os.mkdir"; };
    python.run_as(PLUGIN, R"PY(
os.chdir(RELATIVE_ROOT)
try:
    first_outcome = attempt(lambda: os.mkdir("direct"))
finally:
    os.chdir(OUTSIDE)
)PY");

    CHECK(python.str("first_outcome") == "ok");
    // A relative target is resolved when the event is judged, and the prompt shows both the
    // relative spelling that triggered it and the absolute folder whose grant the approval stores.
    REQUIRE(harness.prompts.size() == 1);
    CHECK(harness.prompts.front().targets == std::vector<std::string>{python.str("RELATIVE_ROOT")});
    CHECK(harness.prompts.front().entries ==
          std::vector<PluginAuditManager::PermissionPromptEntry>{{"direct", python.str("RELATIVE_ROOT")}});
    CHECK(harness.saved_permissions(PLUGIN).fs_create == std::vector<std::string>{python.str("RELATIVE_ROOT")});

    // The saved absolute folder covers another relative operation in the same directory.
    harness.prompts.clear();
    python.run_as(PLUGIN, R"PY(
os.chdir(RELATIVE_ROOT)
try:
    second_outcome = attempt(lambda: os.mkdir("second"))
finally:
    os.chdir(OUTSIDE)
)PY");
    CHECK(python.str("second_outcome") == "ok");
    CHECK(describe(harness.prompts).empty());

    // A relative entry saved by hand never matches: its meaning would move with the working
    // directory. The operation asks again for the canonical folder.
    PluginPermissions relative_only;
    relative_only.fs_create = {"relative-root"};
    harness.save_permissions(PLUGIN, relative_only);
    harness.approve = [](const Prompt&) { return false; };
    harness.prompts.clear();
    python.run_as(PLUGIN, R"PY(
os.chdir(RELATIVE_ROOT)
try:
    third_outcome = attempt(lambda: os.mkdir("third"))
finally:
    os.chdir(OUTSIDE)
)PY");
    CHECK(python.str("third_outcome") == "denied");
    REQUIRE(harness.prompts.size() == 1);
    CHECK(harness.prompts.front().targets == std::vector<std::string>{python.str("RELATIVE_ROOT")});
    CHECK(harness.prompts.front().entries ==
          std::vector<PluginAuditManager::PermissionPromptEntry>{{"third", python.str("RELATIVE_ROOT")}});

    // Delete targets resolve the same way. A relative removal inside the granted folder is
    // allowed without a prompt.
    PluginPermissions delete_root;
    delete_root.fs_delete = {python.str("RELATIVE_ROOT")};
    harness.save_permissions(PLUGIN, delete_root);
    harness.prompts.clear();
    python.run_as(PLUGIN, R"PY(
os.chdir(RELATIVE_ROOT)
try:
    relative_delete_outcome = attempt(lambda: os.remove("victim.txt"))
finally:
    os.chdir(OUTSIDE)
)PY");
    CHECK(python.str("relative_delete_outcome") == "ok");
    CHECK(describe(harness.prompts).empty());

    // Without a grant the prompt names the resolved folder, not the relative spelling.
    harness.save_permissions(PLUGIN, {});
    harness.approve = [](const Prompt&) { return false; };
    harness.prompts.clear();
    python.run_as(PLUGIN, R"PY(
os.chdir(RELATIVE_ROOT)
try:
    relative_delete_denied = attempt(lambda: os.remove("victim2.txt"))
finally:
    os.chdir(OUTSIDE)
)PY");
    CHECK(python.str("relative_delete_denied") == "denied");
    REQUIRE(harness.prompts.size() == 1);
    CHECK(harness.prompts.front().targets == std::vector<std::string>{python.str("RELATIVE_ROOT")});
    CHECK(harness.prompts.front().entries ==
          std::vector<PluginAuditManager::PermissionPromptEntry>{{"victim2.txt", python.str("RELATIVE_ROOT")}});
}

TEST_CASE("A link create grant covers only the destination folder", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    python.exec(R"PY(
SOURCE_FOLDER = os.path.realpath(os.path.join(OUTSIDE, "link-source"))
DEST_FOLDER = os.path.realpath(os.path.join(OUTSIDE, "link-dest"))
os.makedirs(SOURCE_FOLDER)
os.makedirs(DEST_FOLDER)
SOURCE_FILE = os.path.join(SOURCE_FOLDER, "source.txt")
with open(SOURCE_FILE, "w") as f:
    f.write("x")
SYMLINK_DEST = os.path.join(DEST_FOLDER, "symlink.txt")
HARDLINK_DEST = os.path.join(DEST_FOLDER, "hardlink.txt")
SECOND_LINK = os.path.join(DEST_FOLDER, "second-link.txt")
try:
    os.symlink(SOURCE_FILE, os.path.join(OUTSIDE, "probe-link"))
    os.remove(os.path.join(OUTSIDE, "probe-link"))
    SYMLINK_OK = True
except (OSError, NotImplementedError):
    SYMLINK_OK = False
try:
    os.link(SOURCE_FILE, os.path.join(OUTSIDE, "probe-hardlink"))
    os.remove(os.path.join(OUTSIDE, "probe-hardlink"))
    HARDLINK_OK = True
except (OSError, NotImplementedError):
    HARDLINK_OK = False
)PY");
    if (python.eval("SYMLINK_OK") != "True" || python.eval("HARDLINK_OK") != "True")
        SKIP("Symbolic or hard links are unavailable");

    // Only the path being created is a create target; the source the link points at is not. A
    // hardlink still needs read/write access to its source, so grant that separately to keep this
    // test focused on the destination folder grant.
    PluginPermissions source_access;
    source_access.fs_readwrite = {python.str("SOURCE_FILE")};
    harness.save_permissions(PLUGIN, source_access);
    harness.approve = [](const Prompt& prompt) { return prompt.event == "os.symlink" || prompt.event == "os.link"; };
    python.run_as(PLUGIN, R"PY(
symlink_outcome = attempt(lambda: os.symlink(SOURCE_FILE, SYMLINK_DEST))
hardlink_outcome = attempt(lambda: os.link(SOURCE_FILE, HARDLINK_DEST))
)PY");

    CHECK(python.str("symlink_outcome") == "ok");
    CHECK(python.str("hardlink_outcome") == "ok");
    // The source a link points at is not a create target, so the prompt shows only the new entry
    // and the folder the approval saves.
    REQUIRE(harness.prompts.size() == 1);
    CHECK(harness.prompts.front().entries ==
          std::vector<PluginAuditManager::PermissionPromptEntry>{
              {python.str("SYMLINK_DEST"), python.str("DEST_FOLDER")}});
    const PluginPermissions after_links = harness.saved_permissions(PLUGIN);
    REQUIRE(after_links.fs_create.size() == 1);
    CHECK(fs::equivalent(fs::path(after_links.fs_create.front()), fs::path(python.str("DEST_FOLDER"))));
    CHECK(after_links.fs_readwrite == source_access.fs_readwrite);

    // The source folder alone does not authorize creating an entry in another folder.
    PluginPermissions source_only;
    source_only.fs_create = {python.str("SOURCE_FOLDER")};
    harness.save_permissions(PLUGIN, source_only);
    harness.approve = [](const Prompt&) { return false; };
    harness.prompts.clear();
    python.run_as(PLUGIN, "second_outcome = attempt(lambda: os.symlink(SOURCE_FILE, SECOND_LINK))");

    CHECK(python.str("second_outcome") == "denied");
    CHECK_FALSE(fs::exists(fs::path(python.str("SECOND_LINK"))));
    CHECK(harness.saved_permissions(PLUGIN).fs_create == std::vector<std::string>{python.str("SOURCE_FOLDER")});
}

TEST_CASE("A hardlink nested in an approved read/write root needs no extra prompt", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    const fs::path destination = harness.outside.path() / "hardlinked-copy";
    python.globals["HARDLINKED_COPY"] = destination.string();
    python.exec(R"PY(
try:
    PROBE = os.path.join(OUTSIDE, "copytree-hardlink-probe.txt")
    os.link(os.path.join(TREE, "one", "f.txt"), PROBE)
    os.remove(PROBE)
    COPY_HARDLINK_OK = True
except OSError:
    COPY_HARDLINK_OK = False
)PY");
    if (python.eval("COPY_HARDLINK_OK") != "True")
        SKIP("Hard links are unavailable here");

    // copytree is the approved read/write root. os.link is called from its stdlib frames, so the
    // hardlink source inside TREE must not need a separate read/write prompt.
    harness.approve = [](const Prompt& prompt) { return prompt.event == "shutil.copytree"; };
    python.run_as(PLUGIN, "shutil.copytree(TREE, HARDLINKED_COPY, copy_function=os.link)");

    REQUIRE(harness.prompts.size() == 1);
    CHECK(harness.prompts.front().event == "shutil.copytree");
    const fs::path copied_file = destination / "one" / "f.txt";
    REQUIRE(fs::exists(copied_file));
    CHECK(fs::equivalent(copied_file, fs::path(python.str("TREE")) / "one" / "f.txt"));
    CHECK(harness.saved_permissions(PLUGIN).fs_read.empty());
    const std::vector<std::string> expected_readwrite{python.str("TREE"), destination.string()};
    CHECK(harness.saved_permissions(PLUGIN).fs_readwrite == expected_readwrite);
}

TEST_CASE("A hardlink needs read/write permission for a source outside a writable root", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);
    python.globals["DATA_DIR"] = harness.data.dir.string();

    python.exec(R"PY(
SOURCE = os.path.join(OUTSIDE, "hardlink-source.txt")
with open(SOURCE, "w") as f:
    f.write("secret")
DATA_SOURCE = os.path.join(DATA_DIR, "hardlink-data-source.txt")
with open(DATA_SOURCE, "w") as f:
    f.write("data")
DEST_DENIED = os.path.join(DATA_DIR, "hardlink-denied.txt")
DEST_READ_ONLY = os.path.join(DATA_DIR, "hardlink-read-only.txt")
DEST_GRANTED = os.path.join(DATA_DIR, "hardlink-granted.txt")
DEST_FROM_DATA_SOURCE = os.path.join(DATA_DIR, "hardlink-from-data-source.txt")
try:
    PROBE = os.path.join(DATA_DIR, "hardlink-probe.txt")
    os.link(SOURCE, PROBE)
    os.remove(PROBE)
    HARDLINK_OK = True
except OSError:
    HARDLINK_OK = False
)PY");
    if (python.eval("HARDLINK_OK") != "True")
        SKIP("Hard links are unavailable here");

    // The destination sits inside data_dir(), an implicitly writable allowed root. That must not
    // authorize writing through a hardlink to a source outside it.
    python.run_as(PLUGIN, "denied_outcome = attempt(lambda: os.link(SOURCE, DEST_DENIED))");
    CHECK(python.str("denied_outcome") == "denied");
    CHECK_FALSE(fs::exists(fs::path(python.str("DEST_DENIED"))));

    // A read grant is not enough either: the hardlink still has to ask for write access to the
    // source, and denying that prompt leaves the operation blocked.
    PluginPermissions read_only;
    read_only.fs_read = {python.str("SOURCE")};
    harness.save_permissions(PLUGIN, read_only);
    harness.approve = [](const Prompt&) { return false; };
    harness.prompts.clear();
    python.run_as(PLUGIN, "read_only_outcome = attempt(lambda: os.link(SOURCE, DEST_READ_ONLY))");
    CHECK(python.str("read_only_outcome") == "denied");
    CHECK_FALSE(fs::exists(fs::path(python.str("DEST_READ_ONLY"))));
    CHECK(describe(harness.prompts) == "os.link: " + python.str("SOURCE") + "\n");

    // A read/write grant for the source makes the same link creation succeed without another
    // prompt.
    PluginPermissions read_write;
    read_write.fs_readwrite = {python.str("SOURCE")};
    harness.save_permissions(PLUGIN, read_write);
    harness.prompts.clear();
    python.run_as(PLUGIN, "granted_outcome = attempt(lambda: os.link(SOURCE, DEST_GRANTED))");
    CHECK(python.str("granted_outcome") == "ok");
    CHECK(fs::exists(fs::path(python.str("DEST_GRANTED"))));
    CHECK(describe(harness.prompts).empty());

    // A source inside the writable allowed root itself needs no separate grant: writing through
    // the new name stays inside a folder the plugin may already write.
    harness.save_permissions(PLUGIN, {});
    harness.prompts.clear();
    python.run_as(PLUGIN, "data_source_outcome = attempt(lambda: os.link(DATA_SOURCE, DEST_FROM_DATA_SOURCE))");
    CHECK(python.str("data_source_outcome") == "ok");
    CHECK(fs::exists(fs::path(python.str("DEST_FROM_DATA_SOURCE"))));
    CHECK(describe(harness.prompts).empty());
}

TEST_CASE("A recursive delete needs one-time subtree consent instead of a direct folder grant", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    python.exec(R"PY(
RMTREE_PARENT = os.path.realpath(os.path.join(OUTSIDE, "rmtree-parent"))
RMTREE_TARGET = os.path.join(RMTREE_PARENT, "sub")
os.makedirs(RMTREE_TARGET)
for name in ("a.txt", "b.txt"):
    with open(os.path.join(RMTREE_TARGET, name), "w") as f:
        f.write("x")
RMTREE_EXACT_TARGET = os.path.join(RMTREE_PARENT, "exact")
os.makedirs(RMTREE_EXACT_TARGET)
with open(os.path.join(RMTREE_EXACT_TARGET, "one.txt"), "w") as f:
    f.write("x")
)PY");

    // A grant naming the parent folder authorises deleting the direct entry "sub", not walking
    // and deleting what is inside it. The recursive delete asks for subtree consent instead.
    PluginPermissions permissions;
    permissions.fs_delete = {python.str("RMTREE_PARENT")};
    harness.save_permissions(PLUGIN, permissions);
    harness.approve_recursive_delete = [](const Prompt&) { return false; };
    python.run_as(PLUGIN, "denied_outcome = attempt(lambda: shutil.rmtree(RMTREE_TARGET))");

    CHECK(python.str("denied_outcome") == "denied");
    REQUIRE(harness.prompts.size() == 1);
    CHECK(harness.prompts.front().event == "shutil.rmtree");
    CHECK(harness.prompts.front().targets == std::vector<std::string>{python.str("RMTREE_TARGET")});
    // The prompt has no saved folder: recursive-delete consent is for this operation only.
    CHECK(harness.prompts.front().entries ==
          std::vector<PluginAuditManager::PermissionPromptEntry>{{python.str("RMTREE_TARGET"), ""}});
    CHECK(fs::is_directory(fs::path(python.str("RMTREE_TARGET"))));
    CHECK(harness.saved_permissions(PLUGIN).fs_delete == std::vector<std::string>{python.str("RMTREE_PARENT")});

    // A grant naming the recursive target itself is also a direct folder grant, so it does not
    // cover the subtree either.
    harness.prompts.clear();
    permissions.fs_delete = {python.str("RMTREE_EXACT_TARGET")};
    harness.save_permissions(PLUGIN, permissions);
    python.run_as(PLUGIN, "exact_outcome = attempt(lambda: shutil.rmtree(RMTREE_EXACT_TARGET))");
    CHECK(python.str("exact_outcome") == "denied");
    REQUIRE(harness.prompts.size() == 1);
    CHECK(harness.prompts.front().targets == std::vector<std::string>{python.str("RMTREE_EXACT_TARGET")});
    CHECK(fs::exists(fs::path(python.str("RMTREE_EXACT_TARGET")) / "one.txt"));

    // One-time consent lets this operation proceed, including its nested deletes, and stores
    // nothing: the saved direct grant is unchanged.
    harness.prompts.clear();
    harness.approve_recursive_delete = [](const Prompt&) { return true; };
    python.run_as(PLUGIN, "approved_outcome = attempt(lambda: shutil.rmtree(RMTREE_TARGET))");

    CHECK(python.str("approved_outcome") == "ok");
    CHECK_FALSE(fs::exists(fs::path(python.str("RMTREE_TARGET"))));
    CHECK(harness.saved_permissions(PLUGIN).fs_delete == std::vector<std::string>{python.str("RMTREE_EXACT_TARGET")});
}

TEST_CASE("A saved delete tree grant covers a recursive delete without a prompt", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    python.exec(R"PY(
RMTREE_TREE_ROOT = os.path.realpath(os.path.join(OUTSIDE, "rmtree-tree-grant"))
RMTREE_TREE_TARGET = os.path.join(RMTREE_TREE_ROOT, "sub")
os.makedirs(RMTREE_TREE_TARGET)
with open(os.path.join(RMTREE_TREE_TARGET, "a.txt"), "w") as f:
    f.write("x")
RMTREE_FULL_TARGET = os.path.join(RMTREE_TREE_ROOT, "full")
os.makedirs(RMTREE_FULL_TARGET)
with open(os.path.join(RMTREE_FULL_TARGET, "a.txt"), "w") as f:
    f.write("x")
RMTREE_SELF_GRANT = os.path.join(RMTREE_TREE_ROOT, "self-grant")
os.makedirs(RMTREE_SELF_GRANT)
with open(os.path.join(RMTREE_SELF_GRANT, "a.txt"), "w") as f:
    f.write("x")
RMTREE_SELF_COVERED = os.path.join(RMTREE_TREE_ROOT, "self-covered")
os.makedirs(RMTREE_SELF_COVERED)
with open(os.path.join(RMTREE_SELF_COVERED, "a.txt"), "w") as f:
    f.write("x")
)PY");

    PluginPermissions permissions;
    permissions.fs_delete = {python.str("RMTREE_TREE_ROOT") + "/*"};
    harness.save_permissions(PLUGIN, permissions);
    python.run_as(PLUGIN, "tree_outcome = attempt(lambda: shutil.rmtree(RMTREE_TREE_TARGET))");

    CHECK(python.str("tree_outcome") == "ok");
    CHECK(describe(harness.prompts).empty());
    CHECK_FALSE(fs::exists(fs::path(python.str("RMTREE_TREE_TARGET"))));
    CHECK(harness.saved_permissions(PLUGIN).fs_delete ==
          std::vector<std::string>{python.str("RMTREE_TREE_ROOT") + "/*"});

    // Full delete access likewise covers the operation without a prompt.
    permissions.fs_delete = {"*"};
    harness.save_permissions(PLUGIN, permissions);
    harness.prompts.clear();
    python.run_as(PLUGIN, "full_outcome = attempt(lambda: shutil.rmtree(RMTREE_FULL_TARGET))");
    CHECK(python.str("full_outcome") == "ok");
    CHECK(describe(harness.prompts).empty());
    CHECK_FALSE(fs::exists(fs::path(python.str("RMTREE_FULL_TARGET"))));

    // A tree grant rooted at the target itself covers its contents but not the final rmdir of
    // the target: that entry lives in the parent folder. The grant must not be silently extended
    // to remove the directory; consent is asked before the walk starts, so denial leaves the tree
    // intact and approval lets the whole operation finish without a mid-walk prompt.
    permissions.fs_delete = {python.str("RMTREE_SELF_GRANT") + "/*"};
    harness.save_permissions(PLUGIN, permissions);
    harness.prompts.clear();
    harness.approve_recursive_delete = [](const Prompt&) { return false; };
    python.run_as(PLUGIN, "self_grant_denied = attempt(lambda: shutil.rmtree(RMTREE_SELF_GRANT))");
    CHECK(python.str("self_grant_denied") == "denied");
    REQUIRE(harness.prompts.size() == 1);
    CHECK(harness.prompts.front().event == "shutil.rmtree");
    CHECK(harness.prompts.front().targets == std::vector<std::string>{python.str("RMTREE_SELF_GRANT")});
    CHECK(fs::exists(fs::path(python.str("RMTREE_SELF_GRANT")) / "a.txt"));
    CHECK(harness.saved_permissions(PLUGIN).fs_delete ==
          std::vector<std::string>{python.str("RMTREE_SELF_GRANT") + "/*"});

    harness.prompts.clear();
    harness.approve_recursive_delete = [](const Prompt&) { return true; };
    python.run_as(PLUGIN, "self_grant_approved = attempt(lambda: shutil.rmtree(RMTREE_SELF_GRANT))");
    CHECK(python.str("self_grant_approved") == "ok");
    CHECK(harness.prompts.size() == 1); // only the pre-walk consent, no nested prompt
    CHECK_FALSE(fs::exists(fs::path(python.str("RMTREE_SELF_GRANT"))));

    // The same target-rooted tree grant plus a separate grant for the parent covers the root
    // removal too, so the saved grants suffice without a prompt.
    permissions.fs_delete = {python.str("RMTREE_SELF_COVERED") + "/*", python.str("RMTREE_TREE_ROOT")};
    harness.save_permissions(PLUGIN, permissions);
    harness.prompts.clear();
    python.run_as(PLUGIN, "self_covered_outcome = attempt(lambda: shutil.rmtree(RMTREE_SELF_COVERED))");
    CHECK(python.str("self_covered_outcome") == "ok");
    CHECK(describe(harness.prompts).empty());
    CHECK_FALSE(fs::exists(fs::path(python.str("RMTREE_SELF_COVERED"))));
}

TEST_CASE("Recursive delete consent is not persisted and does not follow a symlink out of the subtree", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    python.exec(R"PY(
RMTREE_SCOPE_ROOT = os.path.realpath(os.path.join(OUTSIDE, "rmtree-scope-root"))
RMTREE_OUTSIDE = os.path.realpath(os.path.join(OUTSIDE, "rmtree-scope-outside"))
os.makedirs(RMTREE_SCOPE_ROOT)
os.makedirs(RMTREE_OUTSIDE)
RMTREE_OUTSIDE_FILE = os.path.join(RMTREE_OUTSIDE, "keep.txt")
with open(RMTREE_OUTSIDE_FILE, "w") as f:
    f.write("x")
with open(os.path.join(RMTREE_SCOPE_ROOT, "delete.txt"), "w") as f:
    f.write("x")
try:
    os.symlink(RMTREE_OUTSIDE, os.path.join(RMTREE_SCOPE_ROOT, "alias"), target_is_directory=True)
    RMTREE_SYMLINK_OK = True
except (OSError, NotImplementedError):
    RMTREE_SYMLINK_OK = False
)PY");
    if (python.eval("RMTREE_SYMLINK_OK") != "True")
        SKIP("Cannot create a directory symlink here");

    harness.approve_recursive_delete = [](const Prompt&) { return true; };
    python.run_as(PLUGIN, "scope_outcome = attempt(lambda: shutil.rmtree(RMTREE_SCOPE_ROOT))");

    CHECK(python.str("scope_outcome") == "ok");
    CHECK_FALSE(fs::exists(fs::path(python.str("RMTREE_SCOPE_ROOT"))));
    // The symlink is an entry in the approved folder, not consent to follow it outside.
    CHECK(fs::exists(fs::path(python.str("RMTREE_OUTSIDE_FILE"))));
    CHECK(harness.saved_permissions(PLUGIN).fs_delete.empty());

    // Approval covers one operation only: the same target asks again instead of inheriting a
    // remembered subtree grant.
    python.exec("os.makedirs(RMTREE_SCOPE_ROOT)");
    harness.prompts.clear();
    harness.approve_recursive_delete = [](const Prompt&) { return false; };
    python.run_as(PLUGIN, "second_scope_outcome = attempt(lambda: shutil.rmtree(RMTREE_SCOPE_ROOT))");
    CHECK(python.str("second_scope_outcome") == "denied");
    REQUIRE(harness.prompts.size() == 1);
    CHECK(harness.prompts.front().event == "shutil.rmtree");
    CHECK(harness.saved_permissions(PLUGIN).fs_delete.empty());
}

TEST_CASE("A recursive delete resolves a symlink/.. spelling using platform semantics", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);
    python.globals["DATA_DIR"] = harness.data.dir.string();

    python.exec(R"PY(
RMTREE_ESCAPE_LINK = os.path.join(DATA_DIR, "orca-rmtree-bypass-%d" % os.getpid())
if os.path.lexists(RMTREE_ESCAPE_LINK):
    os.remove(RMTREE_ESCAPE_LINK)
RMTREE_ESCAPE_OUTSIDE = os.path.realpath(os.path.join(OUTSIDE, "rmtree-escape-outside"))
RMTREE_ESCAPE_DEEP = os.path.join(RMTREE_ESCAPE_OUTSIDE, "deep")
# The name the spelling folds to on Win32, where data_dir() is an implicit writable root and the
# delete is expected to run, and the folder POSIX reaches instead once the link is followed. Both
# are folders of their own, so a delete of either one cannot touch the data directory itself.
RMTREE_ESCAPE_NAME = "rmtree-escape-name"
RMTREE_ESCAPE_INSIDE = os.path.join(DATA_DIR, RMTREE_ESCAPE_NAME)
RMTREE_ESCAPE_TARGET = os.path.join(RMTREE_ESCAPE_OUTSIDE, RMTREE_ESCAPE_NAME)
os.makedirs(RMTREE_ESCAPE_DEEP)
os.makedirs(RMTREE_ESCAPE_INSIDE)
with open(os.path.join(RMTREE_ESCAPE_INSIDE, "inside.txt"), "w") as f:
    f.write("x")
os.makedirs(RMTREE_ESCAPE_TARGET)
with open(os.path.join(RMTREE_ESCAPE_TARGET, "keep.txt"), "w") as f:
    f.write("x")
try:
    os.symlink(RMTREE_ESCAPE_DEEP, RMTREE_ESCAPE_LINK, target_is_directory=True)
    RMTREE_ESCAPE_SYMLINK_OK = True
except (OSError, NotImplementedError):
    RMTREE_ESCAPE_SYMLINK_OK = False
# Lexically this folds to DATA_DIR/<escape-name>, inside the implicit writable root. POSIX follows
# the link first, so there it names OUTSIDE/<escape-name> instead.
RMTREE_ESCAPE_PATH = os.path.join(DATA_DIR, os.path.basename(RMTREE_ESCAPE_LINK), "..", RMTREE_ESCAPE_NAME)
)PY");
    if (python.eval("RMTREE_ESCAPE_SYMLINK_OK") != "True")
        SKIP("Cannot create a directory symlink inside the data directory");

    // On POSIX the link is followed before the
    // step-up, so the target is outside the implicit root and the recursive delete must ask first;
    // denied, it must leave both folders in place. Win32 expands the ".." lexically before the file
    // system sees the path, so the same spelling stays inside the implicit writable root: the walk
    // runs without a prompt and removes the isolated folder created for it above, never the link's
    // destination.
    harness.approve_recursive_delete = [](const Prompt&) { return false; };
    python.run_as(PLUGIN, "escape_outcome = attempt(lambda: shutil.rmtree(RMTREE_ESCAPE_PATH))");

    CHECK(harness.saved_permissions(PLUGIN).fs_delete.empty());
    CHECK(fs::exists(fs::path(python.str("RMTREE_ESCAPE_TARGET")) / "keep.txt"));
#if defined(_WIN32)
    CHECK(python.str("escape_outcome") == "ok");
    CHECK(describe(harness.prompts).empty());
    CHECK_FALSE(fs::exists(fs::path(python.str("RMTREE_ESCAPE_INSIDE"))));
#else
    CHECK(python.str("escape_outcome") == "denied");
    REQUIRE(harness.prompts.size() == 1);
    CHECK(harness.prompts.front().event == "shutil.rmtree");
    CHECK(harness.prompts.front().targets == std::vector<std::string>{python.str("RMTREE_ESCAPE_TARGET")});
    CHECK(fs::exists(fs::path(python.str("RMTREE_ESCAPE_INSIDE")) / "inside.txt"));
#endif

    python.exec("os.remove(RMTREE_ESCAPE_LINK)");
}

TEST_CASE("A recursive delete inside an implicit writable root removes symlink entries without prompts", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);
    python.globals["DATA_DIR"] = harness.data.dir.string();

    python.exec(R"PY(
RMTREE_IMPLICIT_TREE = os.path.realpath(os.path.join(DATA_DIR, "rmtree-implicit"))
RMTREE_IMPLICIT_OUTSIDE = os.path.realpath(os.path.join(OUTSIDE, "rmtree-implicit-outside"))
os.makedirs(RMTREE_IMPLICIT_TREE)
os.makedirs(RMTREE_IMPLICIT_OUTSIDE)
RMTREE_IMPLICIT_KEEP = os.path.join(RMTREE_IMPLICIT_OUTSIDE, "keep.txt")
with open(RMTREE_IMPLICIT_KEEP, "w") as f:
    f.write("x")
with open(os.path.join(RMTREE_IMPLICIT_TREE, "a.txt"), "w") as f:
    f.write("x")
try:
    os.symlink(RMTREE_IMPLICIT_OUTSIDE, os.path.join(RMTREE_IMPLICIT_TREE, "alias"), target_is_directory=True)
    RMTREE_IMPLICIT_SYMLINK_OK = True
except (OSError, NotImplementedError):
    RMTREE_IMPLICIT_SYMLINK_OK = False
)PY");
    if (python.eval("RMTREE_IMPLICIT_SYMLINK_OK") != "True")
        SKIP("Cannot create a directory symlink here");

    // data_dir() is an implicit writable root. The recursive delete needs no consent, and the
    // fd-based reads plus the symlink entry must not interrupt or partially delete the walk.
    python.run_as(PLUGIN, "implicit_outcome = attempt(lambda: shutil.rmtree(RMTREE_IMPLICIT_TREE))");

    CHECK(python.str("implicit_outcome") == "ok");
    CHECK(describe(harness.prompts).empty());
    CHECK_FALSE(fs::exists(fs::path(python.str("RMTREE_IMPLICIT_TREE"))));
    CHECK(fs::exists(fs::path(python.str("RMTREE_IMPLICIT_KEEP"))));
    CHECK(harness.saved_permissions(PLUGIN).fs_delete.empty());
}

TEST_CASE("A recursive delete keeps protected paths blocked inside an approved subtree", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    seed_denied_keywords();
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);
    PluginAuditManager& mgr = PluginAuditManager::instance();

    python.exec(R"PY(
RMTREE_PROTECTED_ROOT = os.path.realpath(os.path.join(OUTSIDE, "rmtree-protected-root"))
RMTREE_PROTECTED_DIR = os.path.join(RMTREE_PROTECTED_ROOT, "secrets")
os.makedirs(RMTREE_PROTECTED_DIR)
RMTREE_PROTECTED_FILE = os.path.join(RMTREE_PROTECTED_DIR, "token.txt")
with open(RMTREE_PROTECTED_FILE, "w") as f:
    f.write("x")
with open(os.path.join(RMTREE_PROTECTED_ROOT, "ordinary.txt"), "w") as f:
    f.write("x")
)PY");

    // The subtree consent covers the folder, but the deny registry is checked before any
    // operation coverage, so the protected entry stops the walk.
    harness.approve_recursive_delete = [](const Prompt&) { return true; };
    python.run_as(PLUGIN, "protected_outcome = attempt(lambda: shutil.rmtree(RMTREE_PROTECTED_ROOT))");

    CHECK(python.str("protected_outcome") == "denied");
    CHECK(fs::exists(fs::path(python.str("RMTREE_PROTECTED_FILE"))));
    CHECK(fs::is_directory(fs::path(python.str("RMTREE_PROTECTED_DIR"))));
    CHECK(harness.saved_permissions(PLUGIN).fs_delete.empty());
    AuditViolation violation;
    REQUIRE(mgr.last_violation(violation));
    CHECK(violation.reason == "denied path");
}

TEST_CASE("A delete is scoped by the entry, not by a final symlink's destination", "[audit]")
{
    ScopedTemporaryDir temp{"orca-plugin-audit-delete-scope"};
    const fs::path approved = temp.path() / "approved";
    const fs::path outside = temp.path() / "outside";
    fs::create_directories(approved / "sub");
    fs::create_directories(outside);

    boost::system::error_code ec;
    fs::create_directory_symlink(approved, outside / "link_to_approved", ec);
    if (ec)
        SKIP("Cannot create a directory symlink here");

    const std::string subtree = fs::weakly_canonical(approved).string();

    // An entry below the subtree and the subtree root itself are covered.
    CHECK(PluginAuditManager::delete_target_inside_subtree_for_testing((approved / "sub" / "a.txt").string(), subtree));
    CHECK(PluginAuditManager::delete_target_inside_subtree_for_testing(approved.string(), subtree));

    // Deleting this link acts on the link entry in `outside`, even though its destination is the
    // approved subtree. The destination must not authorise removing the outside entry.
    CHECK_FALSE(PluginAuditManager::delete_target_inside_subtree_for_testing((outside / "link_to_approved").string(),
                                                                            subtree));
    CHECK_FALSE(PluginAuditManager::delete_target_inside_subtree_for_testing(outside.string(), subtree));

    // `link/..` is resolved by the platform, and the platforms differ. Win32 expands "." and ".."
    // lexically, before the file system sees the path, so this spelling is the approved root itself
    // and a link cannot carry the step-up out of the tree. POSIX follows the link first, so there it
    // is `outside/approved`. The check must give the answer the delete reaches on that platform.
    fs::create_directories(outside / "deep");
    fs::create_directories(outside / "approved");
    fs::create_directory_symlink(outside / "deep", temp.path() / "escape_link", ec);
    REQUIRE_FALSE(ec);
#if defined(_WIN32)
    CHECK(PluginAuditManager::delete_target_inside_subtree_for_testing(
        (temp.path() / "escape_link" / ".." / "approved").string(), subtree));
#else
    CHECK_FALSE(PluginAuditManager::delete_target_inside_subtree_for_testing(
        (temp.path() / "escape_link" / ".." / "approved").string(), subtree));
#endif

    // Both readings of the same spelling name the approved root: POSIX reaches it through the link,
    // Win32 by folding. It is covered either way.
    fs::create_directories(temp.path() / "inside");
    fs::create_directory_symlink(temp.path() / "inside", temp.path() / "safe_link", ec);
    REQUIRE_FALSE(ec);
    CHECK(PluginAuditManager::delete_target_inside_subtree_for_testing(
        (temp.path() / "safe_link" / ".." / "approved").string(), subtree));

    // A final ".." names no entry of the subtree: POSIX refuses it as a final component, and Win32
    // folds it to the approved root's parent, which is outside. Either way it fails closed.
    CHECK_FALSE(PluginAuditManager::delete_target_inside_subtree_for_testing((approved / "..").string(), subtree));

    // A link inside the subtree is an entry inside it; deleting the link does not follow it out.
    fs::create_directory_symlink(outside, approved / "link_to_outside", ec);
    REQUIRE_FALSE(ec);
    CHECK(PluginAuditManager::delete_target_inside_subtree_for_testing((approved / "link_to_outside").string(), subtree));
}

TEST_CASE("An implicit writable root contains a target whose tail does not exist yet", "[audit]")
{
#ifndef _WIN32
    SKIP("Covers the Windows status errors for a missing path; POSIX reports it without an error");
#else
    ScopedTemporaryDir temp{"orca-plugin-audit-missing-tail"};
    const fs::path     root    = temp.path() / "root";
    const fs::path     outside = temp.path() / "outside";
    fs::create_directories(root);
    fs::create_directories(outside);

    PluginAuditManager& mgr = PluginAuditManager::instance();
    ScopedPluginAuditContext ctx("test_plugin", "");
    mgr.add_scoped_allowed_root(root);

    // Windows reports a missing path as a status error (ERROR_FILE_NOT_FOUND or
    // ERROR_PATH_NOT_FOUND), not as a successful "does not exist" check, so the existing-prefix walk
    // has to keep the missing tail as written instead of failing. No 8.3 alias is needed here.
    CHECK(is_inside_allowed_root(root / "missing-leaf", root));
    CHECK(is_inside_allowed_root(root / "missing" / "nested" / "file.txt", root));
    CHECK(mgr.delete_target_inside_writable_allowed_root((root / "missing" / "nested" / "file.txt").string()));

    // A missing tail outside the root stays outside.
    CHECK_FALSE(is_inside_allowed_root(outside / "missing-leaf", root));
    CHECK_FALSE(mgr.delete_target_inside_writable_allowed_root((outside / "missing" / "nested").string()));
#endif
}

TEST_CASE("An implicit writable root matches a target spelled with the other 8.3 name form", "[audit]")
{
#ifndef _WIN32
    SKIP("Windows 8.3 short names exist only on Windows");
#else
    ScopedTemporaryDir temp{"orca-plugin-audit-short-names"};
    const fs::path     long_root    = temp.path() / "OrcaAuditShortNameRoot";
    const fs::path     long_sibling = temp.path() / "OrcaAuditShortNameSibling";
    const fs::path     outside      = temp.path() / "outside";
    fs::create_directories(long_root / "sub");
    fs::create_directories(long_sibling);
    fs::create_directories(outside);

    // Python names a path in its long form (os.path.realpath() and os.scandir() both report long
    // names), while the C++ side may hold the short spelling it was started with. Whether a machine
    // spells its temp root short is environment-dependent, so build the alias here and skip when the
    // volume offers none: there is nothing to compare in that case.
    const fs::path short_root = short_name_alias(long_root);
    if (short_root.empty())
        SKIP("This volume does not expose an 8.3 short name for the test folder");
    // An alias on an ancestor alone would not exercise the final-component comparison, so require
    // the test folder itself to have its own alias.
    if (short_root.filename() == long_root.filename())
        SKIP("The test folder has no 8.3 alias of its own on this volume");

    PluginAuditManager& mgr = PluginAuditManager::instance();

    SECTION("a root registered under its short name contains long-spelled targets")
    {
        ScopedPluginAuditContext ctx("test_plugin", "");
        mgr.add_scoped_allowed_root(short_root);

        CHECK(mgr.delete_target_inside_writable_allowed_root((long_root / "sub").string()));
        CHECK(mgr.delete_target_inside_writable_allowed_root(long_root.string()));
        // The same folder spelled short throughout was already comparable before normalization;
        // keep that working rather than trading one spelling pair for the other.
        CHECK(mgr.delete_target_inside_writable_allowed_root(short_root.string()));
        // The tail below an existing prefix has no short name of its own; the existing prefix must
        // still match, so an rmtree target that does not exist yet is judged the same way.
        CHECK(mgr.delete_target_inside_writable_allowed_root((long_root / "not-created-yet").string()));
        CHECK(mgr.delete_target_inside_writable_allowed_root((long_root / "not-created-yet" / "file.txt").string()));

        // Outside stays outside, including a path that only shares the root's name prefix.
        CHECK_FALSE(mgr.delete_target_inside_writable_allowed_root(outside.string()));
        CHECK_FALSE(mgr.delete_target_inside_writable_allowed_root((outside / "not-created-yet").string()));
        CHECK_FALSE(mgr.delete_target_inside_writable_allowed_root(long_sibling.string()));
        CHECK_FALSE(mgr.delete_target_inside_writable_allowed_root(
            (temp.path() / "OrcaAuditShortNameRoot-sibling").string()));
    }

    SECTION("a root registered under its long name contains short-spelled targets")
    {
        ScopedPluginAuditContext ctx("test_plugin", "");
        mgr.add_scoped_allowed_root(long_root);

        CHECK(mgr.delete_target_inside_writable_allowed_root((short_root / "sub").string()));
        CHECK(mgr.delete_target_inside_writable_allowed_root(short_root.string()));
        CHECK(mgr.delete_target_inside_writable_allowed_root((short_root / "not-created-yet").string()));
        CHECK_FALSE(mgr.delete_target_inside_writable_allowed_root(short_name_alias(long_sibling).string()));
    }

    SECTION("the final symlink entry is still judged by where the entry lives")
    {
        ScopedPluginAuditContext ctx("test_plugin", "");
        mgr.add_scoped_allowed_root(short_root);

        boost::system::error_code ec;
        fs::create_directory_symlink(outside, long_root / "link_to_outside", ec);
        if (ec)
            SKIP("Cannot create a directory symlink here");
        fs::create_directory_symlink(long_root, outside / "link_to_root", ec);
        REQUIRE_FALSE(ec);

        // Deleting the link acts on its entry inside the long-spelled root, not on the outside
        // folder it points at, and the short root spelling does not change that.
        CHECK(mgr.delete_target_inside_writable_allowed_root((long_root / "link_to_outside").string()));
        // The reverse link lives outside, so its destination being the root does not make it
        // implicit.
        CHECK_FALSE(mgr.delete_target_inside_writable_allowed_root((outside / "link_to_root").string()));
    }
#endif
}

TEST_CASE("An extended-length spelling names the same location as its plain form", "[audit][Regression]")
{
#ifndef _WIN32
    SKIP("The extended-length path prefix exists only on Windows");
#else
    ScopedTemporaryDir temp{"orca-plugin-audit-extended-paths"};
    const fs::path     root    = temp.path() / "root";
    const fs::path     outside = temp.path() / "outside";
    fs::create_directories(root / "sub");
    fs::create_directories(outside);

    // A plugin's __file__ is spelled the way its sys.path entry was, and a cache past MAX_PATH can
    // only be canonicalized in the extended-length form, while allowed roots are registered plainly.
    // The same location has to compare equal in both spellings, and the boundary has to stay put.
    const fs::path extended_root    = extended_path(root);
    const fs::path extended_outside = extended_path(outside);
    REQUIRE(extended_root != root);
    REQUIRE(extended_outside != outside);

    PluginAuditManager& mgr = PluginAuditManager::instance();

    SECTION("a plain root contains extended-spelled targets")
    {
        ScopedPluginAuditContext ctx("test_plugin", "");
        mgr.add_scoped_allowed_root(root);

        CHECK(is_inside_allowed_root(extended_path(root / "sub" / "file.txt"), root));
        CHECK(is_inside_allowed_root(root, extended_root));
        CHECK(mgr.delete_target_inside_writable_allowed_root(extended_path(root / "sub").string()));
        // A target that does not exist yet is still inside: only the existing prefix is compared.
        CHECK(mgr.delete_target_inside_writable_allowed_root(extended_path(root / "not-created-yet").string()));

        // Outside stays outside in the extended spelling too, including a name that only shares the
        // root's prefix.
        CHECK_FALSE(is_inside_allowed_root(extended_outside / "file.txt", root));
        CHECK_FALSE(mgr.delete_target_inside_writable_allowed_root(extended_outside.string()));
        CHECK_FALSE(is_inside_allowed_root(extended_path(fs::path(root.string() + "_sibling") / "file.txt"), root));
    }

    SECTION("an extended-spelled root contains plain targets")
    {
        ScopedPluginAuditContext ctx("test_plugin", "");
        mgr.add_scoped_allowed_root(extended_root);

        CHECK(is_inside_allowed_root(root / "sub" / "file.txt", extended_root));
        CHECK(is_inside_allowed_root(extended_root, root));
        CHECK(mgr.delete_target_inside_writable_allowed_root((root / "sub").string()));

        CHECK_FALSE(is_inside_allowed_root(outside / "file.txt", extended_root));
        CHECK_FALSE(is_inside_allowed_root(fs::path(root.string() + "_sibling"), extended_root));
    }

    SECTION("device paths that are not drive-letter aliases stay outside")
    {
        ScopedPluginAuditContext ctx("test_plugin", "");
        mgr.add_scoped_allowed_root(root);

        // "\\?\C:\..." is the only device-namespace alias of a lettered path that the comparison
        // rewrites. A volume GUID, a named pipe, the "\\.\" device namespace and the NT "\??\"
        // prefix name other objects, so stripping the prefix would invent a path inside the root;
        // they must stay unmatched, as must an extended UNC spelling of some other share.
        CHECK_FALSE(is_inside_allowed_root(fs::path(L"\\\\?\\Volume{00000000-0000-0000-0000-000000000000}\\file.txt"), root));
        CHECK_FALSE(is_inside_allowed_root(fs::path(L"\\\\?\\PIPE\\orca-audit"), root));
        CHECK_FALSE(is_inside_allowed_root(fs::path(L"\\\\.\\pipe\\orca-audit"), root));
        CHECK_FALSE(is_inside_allowed_root(fs::path(L"\\??\\C:\\Windows\\win.ini"), root));
        CHECK_FALSE(is_inside_allowed_root(fs::path(L"\\\\?\\UNC\\orca-audit-host\\share\\file.txt"), root));
    }
#endif
}

TEST_CASE("An asynchronous recursive delete approval is single-use and bound to its plugin and target", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    const std::string other_plugin = "OtherAuditPlugin";
    harness.add_plugin(PLUGIN);
    harness.add_plugin(other_plugin);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    python.exec(R"PY(
RMTREE_ASYNC_A = os.path.realpath(os.path.join(OUTSIDE, "rmtree-async-a"))
RMTREE_ASYNC_B = os.path.realpath(os.path.join(OUTSIDE, "rmtree-async-b"))
for root in (RMTREE_ASYNC_A, RMTREE_ASYNC_B):
    os.makedirs(root)
    with open(os.path.join(root, "a.txt"), "w") as f:
        f.write("x")
)PY");

    // The posted prompt is answered Yes, but the attempt that raised it was already denied.
    harness.approve_recursive_delete = [](const Prompt&) { return true; };
    PluginAuditManager::set_recursive_delete_prompt_background_for_testing(true);
    python.run_as(PLUGIN, "first_outcome = attempt(lambda: shutil.rmtree(RMTREE_ASYNC_A))");

    CHECK(python.str("first_outcome") == "denied");
    REQUIRE(harness.prompts.size() == 1);
    CHECK(harness.prompts.front().plugin == PLUGIN);
    CHECK(harness.prompts.front().event == "shutil.rmtree");
    CHECK(PluginAuditManager::pending_recursive_delete_approvals_for_testing() == 1);
    CHECK(harness.saved_permissions(PLUGIN).fs_delete.empty());
    CHECK(fs::is_directory(fs::path(python.str("RMTREE_ASYNC_A"))));

    // The first retry consumes the approval and proceeds without another prompt.
    python.run_as(PLUGIN, "retry_outcome = attempt(lambda: shutil.rmtree(RMTREE_ASYNC_A))");
    CHECK(python.str("retry_outcome") == "ok");
    CHECK(harness.prompts.size() == 1);
    CHECK(PluginAuditManager::pending_recursive_delete_approvals_for_testing() == 0);
    CHECK_FALSE(fs::exists(fs::path(python.str("RMTREE_ASYNC_A"))));

    // The approval is single use: a second recursive delete of the same folder asks again.
    python.exec("os.makedirs(RMTREE_ASYNC_A)");
    python.run_as(PLUGIN, "second_outcome = attempt(lambda: shutil.rmtree(RMTREE_ASYNC_A))");
    CHECK(python.str("second_outcome") == "denied");
    CHECK(harness.prompts.size() == 2);
    CHECK(PluginAuditManager::pending_recursive_delete_approvals_for_testing() == 1);

    // A different target cannot spend that approval.
    python.run_as(PLUGIN, "other_target_outcome = attempt(lambda: shutil.rmtree(RMTREE_ASYNC_B))");
    CHECK(python.str("other_target_outcome") == "denied");
    REQUIRE(harness.prompts.size() == 3);
    CHECK(harness.prompts.back().targets == std::vector<std::string>{python.str("RMTREE_ASYNC_B")});
    CHECK(PluginAuditManager::pending_recursive_delete_approvals_for_testing() == 2);

    // Another plugin cannot spend this plugin's pending approval.
    python.run_as(other_plugin, "other_plugin_outcome = attempt(lambda: shutil.rmtree(RMTREE_ASYNC_B))");
    CHECK(python.str("other_plugin_outcome") == "denied");
    REQUIRE(harness.prompts.size() == 4);
    CHECK(harness.prompts.back().plugin == other_plugin);
    CHECK(PluginAuditManager::pending_recursive_delete_approvals_for_testing() == 3);

    // The original plugin still has its own approval for this target and can consume it.
    python.run_as(PLUGIN, "own_retry_outcome = attempt(lambda: shutil.rmtree(RMTREE_ASYNC_B))");
    CHECK(python.str("own_retry_outcome") == "ok");
    CHECK(harness.prompts.size() == 4);
    CHECK(PluginAuditManager::pending_recursive_delete_approvals_for_testing() == 2);
}

TEST_CASE("A pending recursive delete prompt is not posted twice for the same plugin and target", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    python.exec(R"PY(
RMTREE_DEDUP_ROOT = os.path.realpath(os.path.join(OUTSIDE, "rmtree-dedup"))
os.makedirs(RMTREE_DEDUP_ROOT)
with open(os.path.join(RMTREE_DEDUP_ROOT, "a.txt"), "w") as f:
    f.write("x")
)PY");

    harness.approve_recursive_delete = [](const Prompt&) { return true; };
    PluginAuditManager::set_recursive_delete_prompt_background_for_testing(true);
    PluginAuditManager::set_recursive_delete_prompt_deferred_for_testing(true);
    const std::uint64_t generation = PluginAuditManager::recursive_delete_generation_for_testing(PLUGIN);
    python.run_as(PLUGIN, "first_outcome = attempt(lambda: shutil.rmtree(RMTREE_DEDUP_ROOT))");
    CHECK(python.str("first_outcome") == "denied");
    CHECK(harness.prompts.size() == 1);
    CHECK(PluginAuditManager::pending_recursive_delete_prompts_for_testing() == 1);
    CHECK(PluginAuditManager::pending_recursive_delete_approvals_for_testing() == 0);

    // The dialog is still open, so a retry loop must not queue another modal for the same target.
    python.run_as(PLUGIN, "second_outcome = attempt(lambda: shutil.rmtree(RMTREE_DEDUP_ROOT))");
    CHECK(python.str("second_outcome") == "denied");
    CHECK(harness.prompts.size() == 1);
    CHECK(PluginAuditManager::pending_recursive_delete_prompts_for_testing() == 1);

    // Answering the posted prompt stores the single retry approval and releases the dedup key.
    PluginAuditManager::answer_recursive_delete_prompt_for_testing(PLUGIN, python.str("RMTREE_DEDUP_ROOT"), true,
                                                                   generation);
    CHECK(PluginAuditManager::pending_recursive_delete_prompts_for_testing() == 0);
    CHECK(PluginAuditManager::pending_recursive_delete_approvals_for_testing() == 1);

    python.run_as(PLUGIN, "retry_outcome = attempt(lambda: shutil.rmtree(RMTREE_DEDUP_ROOT))");
    CHECK(python.str("retry_outcome") == "ok");
    CHECK(harness.prompts.size() == 1);
    CHECK(PluginAuditManager::pending_recursive_delete_approvals_for_testing() == 0);
    CHECK_FALSE(fs::exists(fs::path(python.str("RMTREE_DEDUP_ROOT"))));
}

TEST_CASE("The pending recursive delete prompt store rejects new prompts at capacity", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    python.exec(R"PY(
RMTREE_CAP_ROOTS = []
for index in range(17):
    root = os.path.realpath(os.path.join(OUTSIDE, "rmtree-cap-%d" % index))
    os.makedirs(root)
    with open(os.path.join(root, "a.txt"), "w") as f:
        f.write("x")
    RMTREE_CAP_ROOTS.append(root)
RMTREE_CAP_OUTCOMES = []
)PY");

    harness.approve_recursive_delete = [](const Prompt&) { return true; };
    PluginAuditManager::set_recursive_delete_prompt_background_for_testing(true);
    PluginAuditManager::set_recursive_delete_prompt_deferred_for_testing(true);
    const std::uint64_t generation = PluginAuditManager::recursive_delete_generation_for_testing(PLUGIN);
    python.run_as(PLUGIN, R"PY(
for root in RMTREE_CAP_ROOTS:
    RMTREE_CAP_OUTCOMES.append(attempt(lambda root=root: shutil.rmtree(root)))
)PY");

    CHECK(python.strings("RMTREE_CAP_OUTCOMES") == std::vector<std::string>(17, "denied"));
    // The first sixteen queue dialogs; the seventeenth is rejected rather than evicting a key
    // whose dialog is already queued. That keeps the dedup set bounded and prevents unlimited
    // dialogs from repeated distinct targets.
    CHECK(harness.prompts.size() == 16);
    CHECK(PluginAuditManager::pending_recursive_delete_prompts_for_testing() == 16);

    // Answering one releases its key and allows that target's retry.
    PluginAuditManager::answer_recursive_delete_prompt_for_testing(
        PLUGIN, python.eval("RMTREE_CAP_ROOTS[0]"), true, generation);
    CHECK(PluginAuditManager::pending_recursive_delete_prompts_for_testing() == 15);
    python.run_as(PLUGIN, "cap_retry_outcome = attempt(lambda: shutil.rmtree(RMTREE_CAP_ROOTS[0]))");
    CHECK(python.str("cap_retry_outcome") == "ok");
    CHECK_FALSE(fs::exists(fs::path(python.eval("RMTREE_CAP_ROOTS[0]"))));
}

TEST_CASE("Revoking permissions invalidates recursive delete approvals and posted prompt tokens", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    python.exec(R"PY(
RMTREE_REVOKE_A = os.path.realpath(os.path.join(OUTSIDE, "rmtree-revoke-a"))
RMTREE_REVOKE_B = os.path.realpath(os.path.join(OUTSIDE, "rmtree-revoke-b"))
for root in (RMTREE_REVOKE_A, RMTREE_REVOKE_B):
    os.makedirs(root)
    with open(os.path.join(root, "a.txt"), "w") as f:
        f.write("x")
)PY");

    PluginAuditManager::set_recursive_delete_prompt_background_for_testing(true);
    harness.approve_recursive_delete = [](const Prompt&) { return true; };
    python.run_as(PLUGIN, "approval_outcome = attempt(lambda: shutil.rmtree(RMTREE_REVOKE_A))");
    CHECK(python.str("approval_outcome") == "denied");
    CHECK(PluginAuditManager::pending_recursive_delete_approvals_for_testing() == 1);

    PluginAuditManager::set_recursive_delete_prompt_deferred_for_testing(true);
    python.run_as(PLUGIN, "pending_outcome = attempt(lambda: shutil.rmtree(RMTREE_REVOKE_B))");
    CHECK(python.str("pending_outcome") == "denied");
    CHECK(PluginAuditManager::pending_recursive_delete_prompts_for_testing() == 1);
    const std::uint64_t stale_generation = PluginAuditManager::recursive_delete_generation_for_testing(PLUGIN);
    const std::size_t  prompts_before       = harness.prompts.size();

    PluginAuditManager::instance().invalidate_recursive_delete_state(PLUGIN);
    CHECK(PluginAuditManager::pending_recursive_delete_approvals_for_testing() == 0);
    CHECK(PluginAuditManager::pending_recursive_delete_prompts_for_testing() == 0);

    // A callback from the old dialog generation must not recreate an approval.
    PluginAuditManager::answer_recursive_delete_prompt_for_testing(PLUGIN, python.str("RMTREE_REVOKE_B"), true,
                                                                   stale_generation);
    CHECK(PluginAuditManager::pending_recursive_delete_approvals_for_testing() == 0);

    // The old approval for A is gone too; the retry is denied and prompts again.
    PluginAuditManager::set_recursive_delete_prompt_deferred_for_testing(false);
    harness.approve_recursive_delete = [](const Prompt&) { return false; };
    python.run_as(PLUGIN, "revoked_retry_outcome = attempt(lambda: shutil.rmtree(RMTREE_REVOKE_A))");
    CHECK(python.str("revoked_retry_outcome") == "denied");
    CHECK(harness.prompts.size() == prompts_before + 1);
    CHECK(PluginAuditManager::pending_recursive_delete_approvals_for_testing() == 0);
    CHECK(fs::exists(fs::path(python.str("RMTREE_REVOKE_A"))));
}

TEST_CASE("An expired recursive delete approval is not reused", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    python.exec(R"PY(
RMTREE_EXPIRED_ROOT = os.path.realpath(os.path.join(OUTSIDE, "rmtree-expired"))
os.makedirs(RMTREE_EXPIRED_ROOT)
with open(os.path.join(RMTREE_EXPIRED_ROOT, "a.txt"), "w") as f:
    f.write("x")
)PY");

    // A negative lifetime makes the approval expired as soon as it is stored, so the retry must
    // prompt again instead of proceeding.
    PluginAuditManager::set_recursive_delete_approval_lifetime_for_testing(std::chrono::milliseconds(-1));
    harness.approve_recursive_delete = [](const Prompt&) { return true; };
    PluginAuditManager::set_recursive_delete_prompt_background_for_testing(true);
    python.run_as(PLUGIN, "first_outcome = attempt(lambda: shutil.rmtree(RMTREE_EXPIRED_ROOT))");
    CHECK(python.str("first_outcome") == "denied");
    CHECK(PluginAuditManager::pending_recursive_delete_approvals_for_testing() == 0);

    python.run_as(PLUGIN, "retry_outcome = attempt(lambda: shutil.rmtree(RMTREE_EXPIRED_ROOT))");
    CHECK(python.str("retry_outcome") == "denied");
    CHECK(harness.prompts.size() == 2);
    CHECK(fs::is_directory(fs::path(python.str("RMTREE_EXPIRED_ROOT"))));
    CHECK(harness.saved_permissions(PLUGIN).fs_delete.empty());
}

TEST_CASE("A recursive delete prompt shows the subtree and states the consent is one-time", "[audit]")
{
    const std::string text =
        PluginAuditManager::format_recursive_delete_permission_prompt_for_testing("PromptPlugin", "/work/tree");

    const std::string expected =
        "Plugin \"PromptPlugin\" is requesting to recursively delete the following folder:\n"
        "/work/tree\n"
        "\n"
        "Approval covers deleting the entries in this folder and everything inside it while this operation runs. "
        "It is not saved as a permission and cannot be reused for another operation. Protected paths stay blocked.";
    CHECK(text == expected);

    // A posted prompt can only be answered for a later retry, so it must not claim the current
    // operation will proceed, and it must name the short lifetime of that retry approval.
    const std::string background =
        PluginAuditManager::format_recursive_delete_permission_prompt_for_testing("PromptPlugin", "/work/tree", false, true);
    const std::string expected_background =
        "Plugin \"PromptPlugin\" is requesting to recursively delete the following folder:\n"
        "/work/tree\n"
        "\n"
        "Approval covers deleting the entries in this folder and everything inside it, and applies to the next "
        "matching attempt within one minute after you approve. The current attempt has already been denied. It is "
        "not saved as a permission and cannot be reused for another operation. Protected paths stay blocked.";
    CHECK(background == expected_background);

    // A filesystem, drive, or network share root gets an explicit warning about its scope.
    const std::string root = PluginAuditManager::format_recursive_delete_permission_prompt_for_testing(
        "PromptPlugin", "/", /*root_scope=*/true, /*background=*/false);
    const std::string expected_root =
        "Plugin \"PromptPlugin\" is requesting to recursively delete the following folder:\n"
        "/\n"
        "\n"
        "This target is a filesystem, drive, or network share root. Approval covers that entire filesystem, drive, "
        "or network share while this operation runs. It is not saved as a permission and cannot be reused for "
        "another operation. Protected paths stay blocked.";
    CHECK(root == expected_root);
}

TEST_CASE("A trailing symlink is judged by its containing folder, not its target", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    python.exec(R"PY(
LINK_PARENT = os.path.realpath(os.path.join(OUTSIDE, "link-parent"))
LINK_TARGET = os.path.realpath(os.path.join(OUTSIDE, "link-target"))
os.makedirs(LINK_PARENT)
os.makedirs(LINK_TARGET)
TARGET_FILE = os.path.join(LINK_TARGET, "target.txt")
with open(TARGET_FILE, "w") as f:
    f.write("x")
LINK = os.path.join(LINK_PARENT, "alias.txt")
DANGLING = os.path.join(LINK_PARENT, "missing-link.txt")
try:
    os.symlink(TARGET_FILE, LINK)
    os.symlink(os.path.join(LINK_TARGET, "missing.txt"), DANGLING)
    SYMLINKS_OK = True
except (OSError, NotImplementedError):
    SYMLINKS_OK = False
)PY");
    if (python.eval("SYMLINKS_OK") != "True")
        SKIP("Cannot create symbolic links here");

    // Deleting a link acts on the link in LINK_PARENT, never on what it points at.
    PluginPermissions permissions;
    permissions.fs_delete = {python.str("LINK_TARGET")};
    harness.save_permissions(PLUGIN, permissions);
    python.run_as(PLUGIN, R"PY(
target_folder_outcome = attempt(lambda: os.remove(LINK))
dangling_target_folder_outcome = attempt(lambda: os.remove(DANGLING))
)PY");

    CHECK(python.str("target_folder_outcome") == "denied");
    CHECK(python.str("dangling_target_folder_outcome") == "denied");
    CHECK(fs::is_symlink(fs::path(python.str("LINK"))));
    CHECK(fs::is_symlink(fs::path(python.str("DANGLING"))));
    CHECK(fs::exists(fs::path(python.str("TARGET_FILE"))));

    // Granting the containing folder allows deleting the links themselves, without touching the
    // link target or creating the dangling link's missing destination.
    permissions.fs_delete = {python.str("LINK_PARENT")};
    harness.save_permissions(PLUGIN, permissions);
    harness.prompts.clear();
    python.run_as(PLUGIN, R"PY(
link_outcome = attempt(lambda: os.remove(LINK))
dangling_link_outcome = attempt(lambda: os.remove(DANGLING))
)PY");

    CHECK(python.str("link_outcome") == "ok");
    CHECK(python.str("dangling_link_outcome") == "ok");
    CHECK_FALSE(fs::is_symlink(fs::path(python.str("LINK"))));
    CHECK_FALSE(fs::is_symlink(fs::path(python.str("DANGLING"))));
    CHECK(fs::exists(fs::path(python.str("TARGET_FILE"))));
    CHECK_FALSE(fs::exists(fs::path(python.str("LINK_TARGET")) / "missing.txt"));
}

#if !defined(_WIN32) && (defined(__linux__) || defined(__APPLE__))
TEST_CASE("A non-default dir_fd is checked against the folder it actually names", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);
    PluginAuditManager& mgr = PluginAuditManager::instance();

    python.exec(R"PY(
FD_GRANTED = os.path.realpath(os.path.join(OUTSIDE, "fd-granted"))
FD_TARGET = os.path.realpath(os.path.join(OUTSIDE, "fd-target"))
FD_DENIED = os.path.join(OUTSIDE, "secrets")
os.makedirs(FD_GRANTED)
os.makedirs(FD_TARGET)
os.makedirs(FD_DENIED)
FD_VICTIM = os.path.join(FD_TARGET, "victim.txt")
with open(FD_VICTIM, "w") as f:
    f.write("x")
# Open the descriptors before entering any plugin audit scope: os.open itself is an
# FsRead/FsReadWrite audited event and must not fail the test before the descriptor is used.
FD_TARGET_FD = os.open(FD_TARGET, os.O_RDONLY)
FD_DENIED_FD = os.open(FD_DENIED, os.O_RDONLY)
)PY");

    // The working directory is granted, but the operations happen in FD_TARGET through dir_fd.
    // Resolving the relative path against the working directory would authorize or deny the
    // wrong folder, so the hook must resolve the descriptor first.
    PluginPermissions permissions;
    permissions.fs_create = {python.str("FD_GRANTED")};
    permissions.fs_delete = {python.str("FD_GRANTED")};
    harness.save_permissions(PLUGIN, permissions);

    python.run_as(PLUGIN, R"PY(
os.chdir(FD_GRANTED)
try:
    plain_outcome = attempt(lambda: os.mkdir("plain"))
    prompted_create = attempt(lambda: os.mkdir("child", dir_fd=FD_TARGET_FD))
    prompted_delete = attempt(lambda: os.remove("victim.txt", dir_fd=FD_TARGET_FD))
finally:
    os.chdir(OUTSIDE)
)PY");

    // A default dir_fd must still be allowed: the audit arguments include the mode, which must
    // not be mistaken for a descriptor.
    CHECK(python.str("plain_outcome") == "ok");
    CHECK(fs::is_directory(fs::path(python.str("FD_GRANTED")) / "plain"));
    // Without a grant for FD_TARGET, the prompt names that actual folder and denial leaves both
    // folders untouched.
    CHECK(python.str("prompted_create") == "denied");
    CHECK(python.str("prompted_delete") == "denied");
    CHECK(describe(harness.prompts) == "os.mkdir: " + python.str("FD_TARGET") + "\n" +
                                            "os.remove: " + python.str("FD_TARGET") + "\n");
    CHECK_FALSE(fs::exists(fs::path(python.str("FD_TARGET")) / "child"));
    CHECK(fs::exists(fs::path(python.str("FD_VICTIM"))));

    // A grant naming the resolved folder covers the operations.
    permissions.fs_create = {python.str("FD_TARGET")};
    permissions.fs_delete = {python.str("FD_TARGET")};
    harness.save_permissions(PLUGIN, permissions);
    harness.prompts.clear();
    python.run_as(PLUGIN, R"PY(
os.chdir(FD_GRANTED)
try:
    granted_create = attempt(lambda: os.mkdir("child", dir_fd=FD_TARGET_FD))
    granted_delete = attempt(lambda: os.remove("victim.txt", dir_fd=FD_TARGET_FD))
    granted_rmdir = attempt(lambda: os.rmdir("child", dir_fd=FD_TARGET_FD))
finally:
    os.chdir(OUTSIDE)
)PY");
    CHECK(python.str("granted_create") == "ok");
    CHECK(python.str("granted_delete") == "ok");
    CHECK(python.str("granted_rmdir") == "ok");
    CHECK(describe(harness.prompts).empty());
    CHECK_FALSE(fs::exists(fs::path(python.str("FD_TARGET")) / "child"));
    CHECK_FALSE(fs::exists(fs::path(python.str("FD_VICTIM"))));

    // The resolved target still goes through the deny registry before any folder grant applies.
    permissions.fs_create = {"*"};
    harness.save_permissions(PLUGIN, permissions);
    harness.prompts.clear();
    python.run_as(PLUGIN, R"PY(
os.chdir(FD_GRANTED)
try:
    denied_create = attempt(lambda: os.mkdir("child", dir_fd=FD_DENIED_FD))
finally:
    os.chdir(OUTSIDE)
)PY");
    CHECK(python.str("denied_create") == "denied");
    CHECK(describe(harness.prompts).empty());
    CHECK_FALSE(fs::exists(fs::path(python.str("FD_DENIED")) / "child"));
    python.exec("os.close(FD_TARGET_FD)\nos.close(FD_DENIED_FD)");
    AuditViolation violation;
    REQUIRE(mgr.last_violation(violation));
    CHECK(violation.reason == "denied path");
}

TEST_CASE("A dir_fd rmtree succeeds when full access allows the resolved folder", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    python.exec(R"PY(
import inspect
RMTREE_DIR_FD_SUPPORTED = "dir_fd" in inspect.signature(shutil.rmtree).parameters
RMTREE_PARENT = os.path.join(OUTSIDE, "rmtree-fd-parent")
RMTREE_TREE = os.path.join(RMTREE_PARENT, "tree")
os.makedirs(RMTREE_TREE)
for name in ("a.txt", "b.txt"):
    with open(os.path.join(RMTREE_TREE, name), "w") as f:
        f.write("x")
# Open outside the plugin scope: os.open is audited and would otherwise need a read grant.
RMTREE_FD = os.open(RMTREE_PARENT, os.O_RDONLY)
)PY");
    if (python.eval("RMTREE_DIR_FD_SUPPORTED") != "True")
        SKIP("This Python's shutil.rmtree has no dir_fd parameter");

    // Full access is the "always allowed" counterpart: descriptor resolution must not turn an
    // otherwise authorized recursive delete into a denial.
    PluginPermissions permissions;
    permissions.full_access = true;
    harness.save_permissions(PLUGIN, permissions);

    python.run_as(PLUGIN, R"PY(
outcome = attempt(lambda: shutil.rmtree("tree", dir_fd=RMTREE_FD))
)PY");

    CHECK(python.str("outcome") == "ok");
    CHECK(describe(harness.prompts).empty());
    CHECK_FALSE(fs::exists(fs::path(python.str("RMTREE_TREE"))));
    python.exec("os.close(RMTREE_FD)");
}
TEST_CASE("A dir_fd recursive delete resolves the target before asking for consent", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    python.exec(R"PY(
import inspect
RMTREE_CONSENT_DIR_FD_SUPPORTED = "dir_fd" in inspect.signature(shutil.rmtree).parameters
RMTREE_CONSENT_PARENT = os.path.realpath(os.path.join(OUTSIDE, "rmtree-consent-fd-parent"))
RMTREE_CONSENT_TREE = os.path.join(RMTREE_CONSENT_PARENT, "tree")
os.makedirs(RMTREE_CONSENT_TREE)
with open(os.path.join(RMTREE_CONSENT_TREE, "a.txt"), "w") as f:
    f.write("x")
RMTREE_CONSENT_FD = os.open(RMTREE_CONSENT_PARENT, os.O_RDONLY)
)PY");
    if (python.eval("RMTREE_CONSENT_DIR_FD_SUPPORTED") != "True")
        SKIP("This Python's shutil.rmtree has no dir_fd parameter");

    // Without a grant, the resolved folder is what the consent prompt shows and approves. The
    // operation must also be able to finish: rmtree's fd-based reads and nested deletes run inside
    // the approved subtree.
    harness.approve_recursive_delete = [](const Prompt&) { return true; };
    python.run_as(PLUGIN, R"PY(
fd_outcome = attempt(lambda: shutil.rmtree("tree", dir_fd=RMTREE_CONSENT_FD))
)PY");

    CHECK(python.str("fd_outcome") == "ok");
    REQUIRE(harness.prompts.size() == 1);
    CHECK(harness.prompts.front().event == "shutil.rmtree");
    CHECK(harness.prompts.front().targets == std::vector<std::string>{python.str("RMTREE_CONSENT_TREE")});
    CHECK_FALSE(fs::exists(fs::path(python.str("RMTREE_CONSENT_TREE"))));
    CHECK(harness.saved_permissions(PLUGIN).fs_delete.empty());
    python.exec("os.close(RMTREE_CONSENT_FD)");
}
#elif !defined(_WIN32)
TEST_CASE("A non-default dir_fd fails closed when the platform cannot resolve it", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);

    python.exec(R"PY(
import inspect
DIR_FD_SUPPORTED = "dir_fd" in inspect.signature(os.mkdir).parameters
FD_GRANTED = os.path.realpath(os.path.join(OUTSIDE, "fd-granted"))
FD_TARGET = os.path.realpath(os.path.join(OUTSIDE, "fd-target"))
os.makedirs(FD_GRANTED)
os.makedirs(FD_TARGET)
FD_VICTIM = os.path.join(FD_TARGET, "victim.txt")
with open(FD_VICTIM, "w") as f:
    f.write("x")
# Open outside the plugin scope so the audited os.open cannot fail before descriptor use.
FD_TARGET_FD = os.open(FD_TARGET, os.O_RDONLY)
)PY");
    if (python.eval("DIR_FD_SUPPORTED") != "True")
        SKIP("This Python does not support dir_fd here");

    // Even a grant naming the descriptor's folder cannot make an unresolvable descriptor safe.
    PluginPermissions permissions;
    permissions.fs_create = {python.str("FD_GRANTED"), python.str("FD_TARGET")};
    permissions.fs_delete = {python.str("FD_GRANTED"), python.str("FD_TARGET")};
    harness.save_permissions(PLUGIN, permissions);

    python.run_as(PLUGIN, R"PY(
os.chdir(FD_GRANTED)
try:
    plain_outcome = attempt(lambda: os.mkdir("plain"))
    created_outcome = attempt(lambda: os.mkdir("child", dir_fd=FD_TARGET_FD))
    removed_outcome = attempt(lambda: os.remove("victim.txt", dir_fd=FD_TARGET_FD))
finally:
    os.chdir(OUTSIDE)
)PY");

    CHECK(python.str("plain_outcome") == "ok");
    CHECK(python.str("created_outcome") == "denied");
    CHECK(python.str("removed_outcome") == "denied");
    CHECK(describe(harness.prompts).empty());
    CHECK_FALSE(fs::exists(fs::path(python.str("FD_TARGET")) / "child"));
    CHECK(fs::exists(fs::path(python.str("FD_VICTIM"))));
    python.exec("os.close(FD_TARGET_FD)");
}
#endif

TEST_CASE("A filesystem wildcard does not override a denied path", "[audit][Python]")
{
    AuditHarness harness;
    if (!harness.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    harness.add_plugin(PLUGIN);
    py::gil_scoped_acquire gil;
    PythonSide python(harness);
    PluginAuditManager& mgr = PluginAuditManager::instance();

    python.exec(R"PY(
DENIED_DIR = os.path.join(OUTSIDE, "secrets")
os.makedirs(DENIED_DIR)
with open(os.path.join(DENIED_DIR, "token.txt"), "w") as f:
    f.write("x")
)PY");

    // A grant wide enough to cover the deny registry's target still loses to the deny.
    PluginPermissions permissions;
    permissions.fs_read = {python.str("OUTSIDE") + "/*"};
    harness.save_permissions(PLUGIN, permissions);

    python.run_as(PLUGIN, "denied_outcome = attempt(lambda: open(os.path.join(DENIED_DIR, 'token.txt')).read())");

    CHECK(python.str("denied_outcome") == "denied");
    CHECK(describe(harness.prompts).empty());
    AuditViolation violation;
    REQUIRE(mgr.last_violation(violation));
    CHECK(violation.reason == "denied path");
}
