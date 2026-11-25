#ifndef slic3r_PluginAuditManager_hpp_
#define slic3r_PluginAuditManager_hpp_

// Via pybind11 so this file requests the same python3xx.lib as everything else.
#include <boost/filesystem/path.hpp>
#include <pybind11/conduit/wrap_include_python_h.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <boost/filesystem.hpp>

namespace Slic3r {

struct PluginPermissions;

// Thread creation exposes no user-supplied target, so the runtime audit event reports this fixed
// target. Legacy sidecars also stored it as the sole entry of their threading target list; the
// reader maps that form onto the boolean permission.
inline constexpr const char* kThreadPermissionTarget = "thread";

struct AuditDecision {
    bool        allowed = true;
    std::string reason;
};

struct AuditViolation {
    std::string            plugin_key;
    std::string            event_name;
    std::string            reason;
};

// A filesystem root a plugin may access while an audit context is active. allow_write is
// false for a root that only grants reads (e.g. the bundled, shared resources folder) --
// a write-shaped event never matches such a root, even though a read-shaped one does.
struct AllowedRoot {
    boost::filesystem::path path;
    bool                    allow_write = true;
};

// The set of CPython audit events PluginAuditManager recognizes, grouped by the kind of
// operation they represent. None means the event isn't one audit_hook() acts on at all.
enum class AuditEventCategory {
    None,
    FsRead,
    FsReadWrite,
    FsCreate,
    FsDelete,
    Http,
    Socket,
    ProcessCreate,
    ProcessReplace,
    Threading,
};

// Returns true if candidate resolves to a path inside allowed_root.
// Uses weakly_canonical and component-wise comparison to reject traversal attacks. On Windows both
// sides also have their 8.3 short names expanded, so the same directory spelled either way still
// compares equal. A candidate whose final component is a dangling symlink, or a path that cannot be
// normalized, is never inside: containment fails closed rather than authorizing the destination a
// write through it would create.
bool is_inside_allowed_root(const boost::filesystem::path& candidate,
                            const boost::filesystem::path& allowed_root);

class PluginAuditManager
{
public:
    static PluginAuditManager& instance();

    // Call once after Py_Initialize to install the global audit hook.
    void install_hook();

    // --- current-plugin context (thread_local) ---
    void        set_current_plugin(const std::string& plugin_key);
    std::string current_plugin() const;
    void        clear_current_plugin();

    // --- current-capability context (thread_local) ---
    // The capability whose method is currently executing, within the current plugin. Empty
    // while a plugin-wide call runs, and during capture (get_name/get_type), where the
    // capability has no cached name yet.
    void        set_current_capability(const std::string& capability_name);
    std::string current_capability() const;
    void        clear_current_capability();

    // --- allowed-roots registry ---
    // allow_write = false registers a read-only root: a read-shaped event inside it is allowed,
    // but a write/create/delete-shaped event is not, so it falls through to the normal
    // prompt-or-deny path instead.
    void add_global_allowed_root(const boost::filesystem::path& root, bool allow_write = true);
    void add_scoped_allowed_root(const boost::filesystem::path& root, bool allow_write = true);

    // --- denied-filenames registry ---
    // Filenames a plugin may never touch, in any directory, regardless of the enclosing allowed
    // root.  A candidate is denied when its filename starts with a
    // registered name, so .bak/.tmp companions are covered by the same entry.
    //
    // The comparison is case-insensitive on every platform, unlike the _WIN32-only iequals
    // in is_inside_allowed_root: the default macOS APFS configuration is case-insensitive
    // too, so `orcaslicer.conf` reaches the real file there.  Over-blocking a genuinely
    // distinct name on Linux is the fail-safe direction and costs nothing real.
    void add_denied_filename(const std::string& filename);

    // The list install_hook() seeds into the deny registry: the app config (both app keys and
    // both extensions) and the cloud refresh token. Exposed so tests seed the exact same set
    // without a live interpreter, so the test and production seeding cannot drift apart.
    static std::vector<std::string> default_denied_filenames();

    // True when candidate's base name starts with a denied name (case-insensitive). No path
    // resolution: laundering a denied file through a symlink, hardlink, subprocess, or Windows
    // 8.3 short name is out of scope (see the design doc). This blocks direct access only.
    bool is_denied_filename(const boost::filesystem::path& candidate) const;

    // --- denied-path-keyword registry ---
    // Keywords that categorically deny a path when a component matches one case-insensitively --
    // e.g. a "secrets" subfolder, a "certificates" folder, a "conf"/"config" directory, or a
    // .conf/.ini file anywhere the plugin can otherwise reach, including inside an allowed root.
    // The components above the app's data_dir() are not considered (see below). This is broader
    // than the exact-name is_denied_filename registry, but it is not a substring match:
    // importable modules such as numpy/__config__.py, configparser.py, sysconfig.py, or user
    // folders such as "Conference" and "Concert" are unrelated names and must stay promptable.
    void add_denied_path_keyword(const std::string& keyword);

    // The list install_hook() seeds into the keyword registry. Exposed so tests seed the exact
    // same set without a live interpreter.
    static std::vector<std::string> default_denied_path_keywords();

    // Exact base names the keyword deny must not fire on. Registering an exempt name narrows
    // the keyword rule only: the exact-name is_denied_filename registry still wins for these.
    void add_keyword_exempt_filename(const std::string& filename);

    // True when any component of candidate's (canonicalized) path matches a registered keyword,
    // case-insensitively. Inside data_dir(), only the components below data_dir() are checked:
    // the app's own data folder may itself sit under a component named like a keyword (Flatpak
    // keeps it under .../config/OrcaSlicer), which must not deny everything inside it. A
    // registered "conf" keyword also blocks .conf/.ini file components.
    bool is_denied_path_keyword(const boost::filesystem::path& candidate) const;

    // The list of exact base names the keyword deny must not fire on, exposed so tests seed
    // from the same source as install_hook().
    static std::vector<std::string> default_keyword_exempt_filenames();

    // True when candidate's base name equals a registered exempt name case-insensitively.
    // Exact equality only: companions such as obn.conf.bak keep matching the keyword rule.
    bool is_keyword_exempt(const boost::filesystem::path& candidate) const;

    // is_denied_filename(candidate) || (is_denied_path_keyword(candidate) &&
    // !is_keyword_exempt(candidate)). Convenience for
    // call sites that only need to know whether a path is categorically off-limits, not which
    // specific rule fired.
    bool is_denied_path(const boost::filesystem::path& candidate) const;

    // --- policy checks ---
    // Shared core for every audited filesystem event.  The deny checks are consulted above the
    // allowed roots, so a denied path is blocked even when it sits inside an allowed root (e.g.
    // data_dir(), which is a global allowed root).
    AuditDecision check_path_access(const boost::filesystem::path& candidate, bool is_write);
    AuditDecision check_open(const std::string& path, const std::string& mode);

    // True when deleting `target` is implicitly allowed because the entry lives in a writable
    // allowed root, or is the root itself. Scope is decided by the canonical containing folder,
    // never by resolving the final component: deleting a symlink acts on the link inside the
    // root, and an outside link pointing into the root is not implicit. This is the FsDelete
    // form of the allowed-root check; it preserves deny handling done earlier in the hook.
    bool delete_target_inside_writable_allowed_root(const std::string& target) const;

    // Ask the user to grant the requested permissions. The request may originate on a plugin
    // load worker, so it goes through the same serialized prompt queue as runtime events; a worker
    // waits for the answer, while a GUI-thread caller only shows it when no other prompt is active
    // and is otherwise rejected without queueing a deferred declaration. Targets already persisted
    // for the plugin are not shown or asked for again. Returns true only when every missing target
    // was granted and persisted; denial aborts the plugin load without adding a permission. A
    // plugin with full access stored in its sidecar is never prompted: the declaration is accepted
    // and no grant is added.
    //
    // A filesystem target may name a directory tree: a trailing "/*" (e.g. "/path/to/folder/*")
    // grants the folder itself and every path beneath it, at any depth. "*" is the only wildcard
    // and is only recognized as the final path component; elsewhere it is an ordinary character.
    // The wildcard applies to fs_read, fs_readwrite, fs_create and fs_delete; it never applies to
    // event grants or the other buckets, and a read grant still never authorizes a write (or the
    // reverse). Denied paths stay denied regardless of a covering wildcard.
    //
    // A relative tree grant is resolved against the working directory when the request is made:
    // the prompt shows, and the sidecar stores, the absolute scope it covers. Read/read-write
    // exact grants and the non-filesystem buckets are stored as requested; create/delete folders
    // are always stored as canonical absolute paths (see below).
    //
    // fs_create and fs_delete name folders, not files. A literal folder entry grants creating or
    // deleting the entries directly inside that folder, never inside a subfolder. A tree entry
    // "<folder>/*" grants the same for that folder and every folder beneath it, at any depth. A
    // request for "*" on its own asks for unrestricted access to that operation; a "*" anywhere
    // else, except as the final component of a tree entry, and any other wildcard, makes the
    // declaration fail closed. A relative folder or tree root is resolved against the working
    // directory and stored in canonical absolute form. A tree rooted at a filesystem, drive, or
    // network share root (for example "/*", "C:\*", or a UNC share root) covers that whole
    // filesystem, drive, or network share; its prompt says so explicitly. Denied paths stay
    // denied. At runtime, approving either operation stores the
    // parent folder of the target (a direct grant, not a tree grant), and the prompt shows that
    // canonical folder rather than the target. A recursive delete (shutil.rmtree) is the
    // exception: a direct folder grant is insufficient, so it needs a saved tree grant covering
    // the target or one-time consent for the canonical subtree. That consent is never stored; see
    // the recursive-delete consent helpers below. A runtime folder whose own name contains "*" is
    // never prompted for or stored, because the sidecar would read it as a tree grant. The GUI
    // thread then lets the operation proceed; a worker-thread approval denies the current
    // operation, which has to be retried.
    //
    // Tree grants are matched on canonical paths: a symlink below the granted folder does not
    // extend the grant outside it, and a grant whose root is a symlink applies to its target. A
    // target whose final component is a dangling symlink is never covered, since a write through
    // it would create the link's target rather than the path the grant describes. Exact grants
    // match verbatim; absolute exact grants also match canonical-equivalent paths, including
    // relative targets resolved against the working directory at the time of the check. For
    // create and delete, a tree grant covers every target whose containing folder is its root or
    // any folder below it.
    bool request_permissions(const std::string& plugin_key, const PluginPermissions& requested);

    // Ask the user to grant network access to the given http(s) URLs. Each URL is reduced to its
    // scheme and host before prompting and persisting, so one approval covers every path, query
    // and port on that host. The grant is recorded as an HTTP grant; network_permission_matches
    // applies it to raw socket events for the same host too. Like request_permissions, the request
    // may originate on a plugin load worker; denial aborts the plugin load without adding a
    // permission.
    bool request_network_permissions(const std::string& plugin_key,
                                     const std::vector<std::string>& urls);

    // Canonical scheme://host form used to store and match network permissions. Returns an empty
    // string unless `url` is an absolute http(s) URL with a host. The path, query, fragment,
    // userinfo and port are discarded; scheme and host are lowercased.
    static std::string normalize_network_url(const std::string& url);

    // True when `target` is a create/delete tree grant: a folder path ending in "/*". "*" on its
    // own is the separate full-access form and returns false, as does any wildcard outside the
    // final component. The declared root may be relative and may contain "." or "..";
    // request_permissions resolves it against the working directory and stores it canonically.
    static bool is_create_delete_tree_grant(const std::string& target);

    // --- recursive delete consent ---
    // A recursive delete (shutil.rmtree) is authorised only by saved delete grants covering the
    // whole operation, or by a one-time consent for that subtree. A plain folder grant is
    // insufficient even when it names the target folder or its parent: it authorises deleting
    // direct entries, not walking and deleting a whole subtree. A tree grant rooted exactly at
    // the target covers the entries inside it but not deleting the target folder itself, which
    // is an entry in the parent; that final removal needs the parent covered separately or the
    // one-time consent. One-time consent is never written to the sidecar.
    //
    // When the prompt cannot run inline (the event is raised off the wx main thread), the current
    // attempt is denied and the dialog is posted. A Yes records a short-lived (one minute),
    // single-use approval bound to the plugin key and canonical subtree root. `add_` and
    // `consume_` are the in-memory store used by that retry path; the first matching retry
    // consumes the approval. They are thread-safe, bound in time, and never authorise a different
    // plugin or target. A prompt already pending for the same pair is not posted again.
    //
    // A permission edit, revocation, or reinstall invalidates the plugin's transient approvals,
    // pending prompt keys, and any queued dialog callback: posted callbacks carry the generation
    // they were created for and become no-ops after invalidation.
    void invalidate_recursive_delete_state(const std::string& plugin_key);
    bool consume_recursive_delete_approval(const std::string& plugin_key, const std::string& canonical_root);
    void add_recursive_delete_approval(const std::string& plugin_key, const std::string& canonical_root);

    // Answers a recursive-delete consent prompt in place of the dialog, for tests.
    // `target` is the canonical subtree root the consent covers. Production never sets it.
    using RecursiveDeletePrompt = std::function<bool(const std::string& plugin_name, const std::string& target)>;
    static void set_recursive_delete_prompt_for_testing(RecursiveDeletePrompt prompt);

    // Simulates a prompt that cannot run inline on the wx main thread: with `background` true the
    // current attempt is denied, and a Yes from the testing prompt records a retry approval.
    static void set_recursive_delete_prompt_background_for_testing(bool background);

    // Returns the recursive-delete prompt exactly as the dialog receives it, for tests.
    static std::string format_recursive_delete_permission_prompt_for_testing(const std::string& plugin_name,
                                                                             const std::string& target,
                                                                             bool root_scope = false,
                                                                             bool background = false);

    // Test-only lifecycle hooks: clear pending retry approvals, shorten their lifetime, or read
    // how many are pending. Production never calls them.
    static void clear_recursive_delete_approvals_for_testing();
    static void set_recursive_delete_approval_lifetime_for_testing(std::chrono::milliseconds lifetime);
    static std::size_t pending_recursive_delete_approvals_for_testing();

    // Test-only control for the posted-prompt path. With `deferred` true, a background request
    // records its deduplication key but leaves it pending instead of treating the testing prompt's
    // answer as final; answer_recursive_delete_prompt_for_testing() then completes it, as the real
    // dialog callback would. `generation` is the token read when the prompt was posted; answering
    // with a stale token does not store an approval.
    static void set_recursive_delete_prompt_deferred_for_testing(bool deferred);
    static std::uint64_t recursive_delete_generation_for_testing(const std::string& plugin_key);
    static void answer_recursive_delete_prompt_for_testing(const std::string& plugin_key,
                                                           const std::string& canonical_root, bool granted,
                                                           std::uint64_t generation);
    static std::size_t pending_recursive_delete_prompts_for_testing();

    // Exposes the entry-scope predicate used for nested deletes without requiring a live Python
    // frame. The subtree must be canonical and absolute.
    static bool delete_target_inside_subtree_for_testing(const std::string& target,
                                                         const std::string& canonical_subtree);

    // True when a granted scheme://host entry covers `target`. `target` is a URL (urllib.Request),
    // a bare host (http.client.connect, socket.getaddrinfo) or an address as exposed by the audit
    // event (a hostname, a host:port pair, or a legacy persisted tuple such as "('host', 443)").
    // URL targets must match both scheme and host. Canonical scheme://host grants cover all paths;
    // older full-URL grants remain exact. A bare host carries no scheme, so it matches the host of
    // a canonical scheme://host grant only; a full-URL grant does not authorize raw sockets to its
    // host, and a bare host never matches a different host.
    static bool network_permission_matches(const std::vector<std::string>& granted,
                                           const std::string& target);

    void report_violation(const AuditViolation& violation);
    bool audit_denial_pending() const;
    void clear_audit_denial();
    void clear_last_violation();
    bool last_violation(AuditViolation& violation) const;

    // --- operation-root frames ---
    // One stdlib operation fires several nested audit events as it passes through lower layers:
    // urlopen() raises urllib.Request, then http.client and socket events. When an operation's
    // own root event is granted (by a saved grant or the user), the stdlib frame that raised it
    // is held, and a later event whose stack reaches that frame through stdlib frames only is
    // part of the same operation, so it is allowed without a prompt of its own. Plugin or
    // library code the operation calls back into is judged on its own. A frame is let go once it
    // returns, a suspended generator's (os.walk) when the audit scope ends. Letting a frame go may
    // run finalizers of objects its locals held; they are judged as the plugin and never prompt.
    //
    // The root's category is recorded too. A delete root (shutil.rmtree) does not cover nested
    // delete events by itself: an exact folder grant covers only its direct entries, so each
    // nested delete is judged on its own, while a tree grant covering the target may still allow
    // it. One-time recursive-delete consent records its canonical subtree on the frame, which
    // covers nested deletes whose target stays inside that subtree. A read/write root (copytree,
    // unpack_archive, move) still covers the create/delete events it performs.
    // `approved_delete_subtree`, when non-empty, is the canonical subtree a one-time recursive
    // delete consent covers.
    void record_root_frame(AuditEventCategory category,
                           const boost::filesystem::path& approved_delete_subtree = {});
    // `allow_delete_roots` is false for a delete event: a frame recorded for a delete root then
    // does not cover it, while a read/write root still does.
    bool covered_by_root_frame(bool allow_delete_roots = true) const;
    // True when every delete target is inside the subtree of a delete root frame recorded with a
    // subtree scope (one-time consent or an implicit writable root), and the stack between the
    // event and that frame is stdlib code only. This keeps the scope to the approved operation
    // and its subtree; plugin callbacks the operation invokes are judged on their own. Protected
    // paths are denied before this check.
    bool delete_target_inside_approved_subtree(const std::vector<std::string>& targets) const;
    bool releasing_frames() const;

    // True when code from `filename` (a code object's co_filename) is the interpreter's own
    // stdlib: the only code a root frame's coverage passes through. Valid after install_hook().
    static bool is_stdlib_file(const std::string& filename);

    // One target a runtime permission prompt covers: the path that triggered the prompt and, for
    // create and delete, the folder a Yes saves as the persistent grant.
    struct PermissionPromptEntry {
        std::string target;
        std::string grant_folder;

        bool operator==(const PermissionPromptEntry& other) const
        {
            return target == other.target && grant_folder == other.grant_folder;
        }
    };

    // Answers permission prompts in place of the dialog, for tests. `targets` are the persisted
    // grants; `entries` pairs each runtime create/delete target with the folder a Yes saves.
    // Production never sets it.
    using PermissionPrompt = std::function<bool(const std::string& event_name, const std::vector<std::string>& targets,
                                                const std::vector<PermissionPromptEntry>& entries)>;
    static void set_permission_prompt_for_testing(PermissionPrompt prompt);

    // Returns the runtime create/delete prompt exactly as the dialog receives it, for tests.
    // `entries` pairs each triggering target with the folder a Yes saves.
    static std::string format_create_delete_permission_prompt_for_testing(
        AuditEventCategory category, const std::string& plugin_name,
        const std::vector<PermissionPromptEntry>& entries);

    // Answers the combined permission-request prompt in place of the dialog, receiving the message
    // it would show. Tests run without a wxApp, so this seam also stands in for the app check.
    // Production never sets it.
    using PermissionMessagePrompt = std::function<bool(const std::string& message)>;
    static void set_permission_message_prompt_for_testing(PermissionMessagePrompt prompt);

    // Drops runtime audit prompts that have not been shown yet, and cancels any blocked worker
    // waiting on one (including the prompt currently on screen: its late answer is ignored). Called
    // when the application shuts down so a queued prompt cannot open after the GUI is gone, and
    // when a plugin is unloaded or its in-flight load is cancelled so its prompts cannot outlive it.
    static void discard_pending_permission_prompts();
    static void discard_pending_permission_prompts_for(const std::string& plugin_key);

    bool verbose_events = true;

private:
    friend class ScopedPluginAuditContext;

    PluginAuditManager() = default;

    // A stdlib frame that raised an operation's own root event. The vector that holds these owns
    // one strong reference per frame. delete_root marks a frame recorded for a delete root: such
    // a frame does not cover a nested delete by itself (see covered_by_root_frame), unless it also
    // records the canonical subtree of a one-time recursive-delete consent. The mark is sticky
    // once set, even if the same frame later raises another root event.
    struct RootFrame
    {
        PyObject*                    frame;       // strong reference
        bool                         delete_root;
        boost::filesystem::path      approved_delete_subtree; // empty unless one-time consent
    };

    static int audit_hook(const char* event, PyObject* args, void* user_data);

    // True for paths inside the interpreter's stdlib, excluding site-packages. Unlike
    // is_stdlib_file(), this query is not cached because filesystem paths may resolve through
    // symlinks that can change.
    static bool is_stdlib_path(const boost::filesystem::path& path);

    // Drops the references in `frames`, keeping the caller's error and denial state intact.
    void release_frames(std::vector<RootFrame>& frames);

    static thread_local std::string                   m_current_plugin_key;
    static thread_local std::string                   m_current_capability_name;
    static thread_local std::vector<AllowedRoot>       m_scoped_allowed_roots;
    static thread_local std::vector<RootFrame>         m_root_frames;
    static thread_local int                            m_releasing_frames;
    static thread_local bool                           m_audit_denial_pending;
    static thread_local bool                           m_has_last_violation;
    static thread_local AuditViolation                 m_last_violation;

    // mutable: is_denied_filename() is a const query that must lock.
    mutable std::mutex m_mutex;

    std::vector<AllowedRoot>             m_global_allowed_roots;
    std::vector<std::string>             m_denied_filenames;
    std::vector<std::string>             m_denied_path_keywords;
    std::vector<std::string>             m_keyword_exempt_filenames;
    std::unordered_map<std::string, std::unordered_set<std::string>> m_approved_call_sites; // plugin_key -> call-site ids
};

// RAII guard that sets the current plugin key and capability name, restoring the previous
// pair on scope exit. `capability_name` may be empty for calls that are not scoped to a
// single capability.
//
// Each scope owns the root frames recorded inside it and lets them go, as its own plugin, before
// restoring the outer context. That needs the GIL whenever a frame is still held, so construct
// the scope after the GIL guard, to be destroyed before it.
class ScopedPluginAuditContext
{
public:
    explicit ScopedPluginAuditContext(
        const std::string&                   plugin_key,
        const std::string&                   capability_name = {});

    ~ScopedPluginAuditContext();

    ScopedPluginAuditContext(const ScopedPluginAuditContext&)            = delete;
    ScopedPluginAuditContext& operator=(const ScopedPluginAuditContext&) = delete;

private:
    std::string                                m_plugin_key;
    std::string                                m_capability;
    std::string                                m_previous_id;
    std::string                                m_previous_capability;
    std::vector<AllowedRoot>                   m_previous_scoped_roots;
    std::vector<PluginAuditManager::RootFrame> m_previous_root_frames;
};

// What a queued prompt asks the user. A runtime event, a create/delete target and a recursive
// delete consent can all grant what they show; a load-time declaration grants every bucket it
// lists.
enum class AuditPromptKind {
    RuntimeEvent,
    DeclaredPermissions,
    RecursiveDeleteConsent,
};

// The plugin lifecycle facts the prompt drain consults. Plain data so the lifetime decision is
// testable without a GUI or a live plugin system.
struct AuditPromptPluginState
{
    bool loaded           = false;
    bool load_in_progress = false;
    bool load_cancelled   = false;
    bool load_failed      = false;
    bool shutting_down    = false;
};

// True when a queued prompt may still be shown and its answer persisted. An ordinary load
// failure leaves a runtime prompt actionable: the event that was denied is what the user can
// approve now, and the saved grant lets a retry load succeed. Explicit cancellation and unload,
// as well as application shutdown, make a prompt ineligible; those paths also discard its queue
// entries.
bool prompt_plugin_is_available(const AuditPromptPluginState& state, AuditPromptKind kind);

// One permission bucket of a load-time declaration. Kept plain (no wx types) so the queue can be
// owned by production code that outlives static destruction.
struct AuditPromptBucket
{
    AuditEventCategory       category = AuditEventCategory::None;
    std::vector<std::string> targets;

    bool operator==(const AuditPromptBucket& other) const;
};

// Resolves a blocked load worker exactly once. The queue may cancel a request from another thread
// (plugin unload, shutdown) while the GUI thread is still showing its dialog; the second resolve
// must be a no-op. Held by shared_ptr so a request copy and the queue's active slot share it.
struct AuditPromptCompletion
{
    void resolve(bool value);

    std::future<bool> get_future();

private:
    std::mutex         m_mutex;
    std::promise<bool> m_promise;
    bool               m_resolved = false;
};

// One audit decision awaiting a user answer. `keys` is what a Yes persists; `shown` is the target
// list a prompt without entries lists (empty for an event that is granted by name). `entries`
// pairs each runtime create/delete target with the folder a Yes saves, so a queued prompt shows
// the same triggering paths as an inline one.
struct AuditPromptRequest
{
    AuditPromptKind          kind = AuditPromptKind::RuntimeEvent;
    AuditEventCategory       category = AuditEventCategory::None;
    std::string              plugin_key;
    std::string              plugin_name;
    std::string              event_name;
    bool                     by_event = false;
    std::vector<std::string> keys;
    std::vector<std::string> shown;
    std::vector<PluginAuditManager::PermissionPromptEntry> entries;
    // DeclaredPermissions only: the missing buckets the combined dialog lists.
    std::vector<AuditPromptBucket> buckets;
    // DeclaredPermissions only: whether the sidecar was readable when the declaration was read.
    // The queued write must not recreate a sidecar that was readable then but has since
    // disappeared; only one absent at both reads is treated as a new install to seed.
    bool sidecar_was_readable = false;
    // RecursiveDeleteConsent only: the canonical subtree root in `shown`, whether it is a
    // filesystem/drive/share root, and the permission generation the answer is valid for. The
    // consent is one-time and never persisted; a generation change invalidates the answer.
    bool          root_scope = false;
    std::uint64_t recursive_delete_generation = 0;
    // Set only when a worker is blocked waiting for the answer.
    std::shared_ptr<AuditPromptCompletion> completion;
};

// Serializes audit prompts on the GUI thread. At most one request is active: the next one is
// handed out by take_next() only after the previous dialog has returned from ShowModal(), so a
// modal dialog's nested event loop cannot open another audit prompt.
//
// Thread-safe: audit events enqueue from plugin worker threads, the GUI thread drains. Requests
// are never merged unless they are exactly equivalent, so coalescing cannot widen a grant.
class AuditPromptQueue
{
public:
    enum class Admission {
        ShowNow,   // idle and allowed to prompt here: present this request, then complete()
        Queued,    // a prompt is active or waiting: the current audit event stays denied
        Duplicate, // an equivalent request is already active or waiting: this one is dropped
        Rejected   // a synchronous-only caller found the queue busy, or the bounded
                   // recursive-delete consent set is at capacity: nothing was stored
    };

    // Presents a request synchronously only when the queue is idle. Busy callers are rejected
    // atomically instead of leaving a deferred prompt behind: used by GUI-thread callers whose
    // operation is already being denied and cannot wait for a later Yes.
    Admission admit_synchronous(AuditPromptRequest request);

    // `may_show_now` is true only on the GUI thread: it lets an idle queue hand the request back
    // for a synchronous prompt instead of queueing it for a later GUI callback.
    Admission admit(AuditPromptRequest request, bool may_show_now);

    // Claims the oldest queued request when no prompt is active, and marks it active.
    std::optional<AuditPromptRequest> take_next();

    // Claims the active answer for persistence. Cancellation and commit are mutually exclusive:
    // once this returns true, discard_all()/discard_plugin() leave the request alone so a user's
    // accepted answer cannot be dropped midway through the sidecar write. A request already
    // cancelled (shutdown or unload won the race) returns false and must not be persisted.
    bool begin_commit();

    // Marks the active request done. Returns true when it was cancelled while its dialog was up,
    // so the caller must not persist an accepted grant. Idempotent.
    bool complete();

    bool has_pending() const;

    bool idle() const;

    std::size_t pending_count() const;

    // Drops every queued request and cancels any blocked worker. An active prompt stays on screen
    // (it may be mid-modal), but its completion is released now and its late answer must not be
    // persisted. A commit already begun is left to finish. Used on application shutdown.
    void discard_all();

    // Drops queued requests for one plugin and cancels a blocked worker of that plugin. An active
    // prompt is cancelled but stays on screen until its dialog returns, unless its commit already
    // started. Used when a plugin is unloaded or removed.
    void discard_plugin(const std::string& plugin_key);

    // Drops queued recursive-delete consents for one plugin when its permission state changes.
    // An active consent dialog cannot be withdrawn; the generation it carries makes its late
    // answer a no-op.
    void discard_recursive_delete_consents(const std::string& plugin_key);

private:
    // Equality over what the prompt asks and what a Yes would grant, plus the plugin it is
    // granted to. Distinct categories, plugins, targets, triggering paths, or event grants are
    // never considered equivalent, so coalescing cannot hide a prompt or widen a grant.
    static bool same_request(const AuditPromptRequest& a, const AuditPromptRequest& b);

    bool contains_locked(const AuditPromptRequest& request) const;

    // Drops queued requests matching `matches` and cancels the active request when it matches and
    // its commit has not begun. Returns the blocked workers' completions in FIFO order. The caller
    // resolves them after this returns, never under m_mutex: the wake-up can re-enter the queue.
    std::vector<std::shared_ptr<AuditPromptCompletion>>
    discard_matching(const std::function<bool(const AuditPromptRequest&)>& matches);

    mutable std::mutex                m_mutex;
    std::deque<AuditPromptRequest>    m_pending;
    std::optional<AuditPromptRequest> m_active;
    bool                              m_active_cancelled = false;
    bool                              m_active_committing = false;
};

// The process-wide queue the drain and the explicit discard calls use. Exposed so a test can put
// a request behind an active prompt and then exercise PluginManager's unload/cancel wiring
// against the real queue. Production reaches it only through PluginAuditManager.
AuditPromptQueue& runtime_prompt_queue_for_testing();

} // namespace Slic3r

#endif // slic3r_PluginAuditManager_hpp_
