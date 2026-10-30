#ifndef __INFILL_FACTORY_HPP__
#define __INFILL_FACTORY_HPP__

#include "InfillPurpose.hpp"

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

class Fill;
enum InfillPattern : int;

using InfillFactoryFn = std::function<std::unique_ptr<Fill>()>;

// One registered infill pattern. `id` is the stable, nonlocalized key stored in one of the infill
// pattern settings; `display_name` is only for presentation. Built-in patterns declare their key,
// label and supported surfaces in InfillFactory; a plugin pattern names the exact project-visible
// identity that owns it, so a bare pattern ID can never be matched against a different provider.
struct InfillInfo
{
    std::string     id;
    std::string     display_name;
    // Owning capability of a plugin pattern. Both are empty for a built-in pattern.
    std::string     plugin_key;
    std::string     capability_name;
    // Surface purposes this pattern can fill. A built-in pattern's list is derived from the pattern
    // settings that offer its key; a plugin pattern's list is the descriptor's supported_surfaces.
    // A pattern may serve several purposes, but never one it does not declare.
    std::vector<InfillSurfacePurpose> supported_surfaces;
    InfillFactoryFn factory;
    // Opaque keep-alive owned by the layer that published a plugin pattern. The registry never
    // reads it; it exists so a Selection can hold the provider (and therefore its module) alive for
    // the whole slicing job. Empty for built-in patterns and for native providers that own no
    // Python objects. The publishing layer guarantees that the plugin manager still holds its own
    // provider reference until every lease has been released, so dropping a lease never triggers the
    // final, GIL-requiring release of a Python object.
    std::shared_ptr<void> provider_lease;

    bool is_plugin() const { return !capability_name.empty(); }

    bool supports_surface(InfillSurfacePurpose purpose) const
    {
        return std::find(supported_surfaces.begin(), supported_surfaces.end(), purpose) != supported_surfaces.end();
    }
};

// Registry of the Fill implementations a pattern ID can create. Built-ins are registered lazily
// from the explicit metadata switch in InfillFactory; plugin patterns are registered by the plugin
// layer while an enabled capability is loaded.
//
// A plugin entry holds the exact provider identity, a weak factory reference, and an opaque
// provider lease. The lease is held for as long as the pattern is published; while a slicing job
// pins the pattern it also keeps the capability and its Python module alive, even across an unload
// request. A later factory call fails once the capability is gone instead of resolving to another
// provider.
//
// A Selection pins one active pattern for the duration of a slicing job. While a pattern is pinned,
// unregistering it retires the entry instead of erasing it, and no other registration may reuse the
// ID. That keeps a job's selected provider stable even if a plugin is unloaded or another provider
// is loaded while fill workers are still running.
//
// All registry operations are thread safe. A factory is invoked without the registry lock held, so
// a factory may query the registry itself.
class InfillFactory
{
public:
    class Selection
    {
    public:
        Selection() = default;
        Selection(Selection&& other) noexcept;
        Selection& operator=(Selection&& other) noexcept;
        ~Selection();

        Selection(const Selection&)            = delete;
        Selection& operator=(const Selection&) = delete;

        explicit operator bool() const { return m_info != nullptr; }
        const std::string& id() const;

        // Release the pin early. Safe to call more than once; the destructor does the same.
        void reset();

    private:
        friend class InfillFactory;
        explicit Selection(std::shared_ptr<const InfillInfo> info) : m_info(std::move(info)) {}

        // Holding the registry entry (rather than only its ID) keeps the provider lease alive for
        // the job, so an unload cannot take the Python module away from a selection that may still
        // create Fills.
        std::shared_ptr<const InfillInfo> m_info;
    };

    // Stable config key of a built-in pattern, or empty for an internal-only fill (and for ipPlugin).
    // The key, display label and supported surfaces are all declared by the explicit built-in
    // metadata switch in the implementation; nothing is inferred from PrintConfig at runtime.
    static std::string builtin_infill_key(InfillPattern pattern);

    // Register every built-in pattern under its explicit config key. Idempotent.
    static void register_all_infills();

    // Register one built-in pattern. Returns false when the id is already registered by a plugin
    // pattern or by a different built-in entry. `supported_surfaces` is the pattern's purpose
    // metadata; it is validated when a job pins a plugin pattern and is otherwise informational.
    static bool register_infill(const std::string& id,
                                const std::string& display_name,
                                std::vector<InfillSurfacePurpose> supported_surfaces,
                                InfillFactoryFn    infill_factory);

    // Register one plugin pattern. The owner pair and at least one supported surface must be
    // nonempty. Re-registering the same owner refreshes its descriptor and makes the ID selectable
    // again (for example when a capability is re-enabled), keeping any live pin and the same entry.
    // A different owner, a built-in id, or a built-in pattern still registered under the id is
    // rejected. No partial state is kept on failure.
    //
    // `provider_lease` is an opaque keep-alive that the registry holds while the pattern is
    // published and, through a Selection, for the lifetime of a pinned job. The publishing layer
    // supplies it; libslic3r never inspects it.
    static bool register_plugin_infill(const std::string&    id,
                                       const std::string&    display_name,
                                       const std::string&    plugin_key,
                                       const std::string&    capability_name,
                                       std::vector<InfillSurfacePurpose> supported_surfaces,
                                       std::shared_ptr<void> provider_lease,
                                       InfillFactoryFn       infill_factory);

    // Remove every pattern owned by this capability. An entry with a live Selection is retired:
    // new selections fail, while an already-selected job may still attempt to create its Fill. The
    // retired entry keeps the provider lease until the last pin is released.
    static void unregister_plugin_infills(const std::string& plugin_key,
                                          const std::string& capability_name);

    // Change the owner identity of a capability's published patterns without dropping active
    // selections. Used when PluginManager rekeys a loaded package (for example cloud to local).
    static void rekey_plugin_infills(const std::string& old_plugin_key,
                                     const std::string& new_plugin_key,
                                     const std::string& capability_name);

    // True while a slicing job still pins a pattern owned by this exact capability. Callers use it
    // to distinguish a job lease (which must be drained, never force-dropped) from a short-lived
    // call reference.
    static bool has_pinned_infills(const std::string& plugin_key,
                                   const std::string& capability_name);

    // True for any registered id, including a retired entry that is still pinned. Used to reject
    // an ID collision while an earlier provider is still in use.
    static bool is_infill_registered(const std::string& id);

    // Active (selectable) entry, or null when the id is unknown or retired.
    static std::shared_ptr<const InfillInfo> find_infill(const std::string& id);

    // Active entries only. Order is unspecified.
    static std::vector<InfillInfo> get_registered_infills();

    // Create the pattern registered under id, or nullptr when it is unknown or its capability has
    // gone away. This is the only creation path used by slicing; the caller passes no provider
    // reference because the registry entry already identifies the exact owner.
    static std::unique_ptr<Fill> create_infill_by_id(const std::string& id);

    // Pin one active plugin pattern to the exact owner the project manifest references and every
    // purpose of the settings that selected it. Fails without pinning when the id is unknown,
    // retired, not plugin-provided, owned by another capability, or does not declare any of
    // `purposes`. The returned Selection must outlive every Fill created for that job.
    static Selection select_infill(const std::string& id,
                                   const std::string& plugin_key,
                                   const std::string& capability_name,
                                   const std::vector<InfillSurfacePurpose>& purposes,
                                   std::string&       error);

private:
    InfillFactory()                                = delete;
    ~InfillFactory()                               = delete;
    InfillFactory(const InfillFactory&)            = delete;
    InfillFactory& operator=(const InfillFactory&) = delete;

    static void release_selection(const std::string& id);
};

} // namespace Slic3r

#endif
