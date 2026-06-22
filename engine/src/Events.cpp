// Events — type-erased pub/sub bus.
//
// Working implementation (not a stub) because Phase 0.5 expects the bus to
// be live during 0.4 rerouting — slicing/loading callbacks already need a
// home. Dispatch is std::type_info-keyed; subscribers fire on the publisher
// thread.

#include "orca/Events.hpp"

#include <cstdint>
#include <mutex>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace orca {

struct Events::Impl {
    std::mutex mu;
    std::uint64_t next_id = 1;

    struct Sub {
        SubscriptionId id;
        std::function<void(const void*)> handler;
    };

    std::unordered_map<std::type_index, std::vector<Sub>> by_type;
    std::unordered_map<SubscriptionId, std::type_index>   id_to_type;
};

Events::Events() : impl_(std::make_unique<Impl>()) {}
Events::~Events() = default;

SubscriptionId Events::subscribe_erased(
    const std::type_info& type,
    std::function<void(const void*)> handler)
{
    std::lock_guard<std::mutex> lock(impl_->mu);
    SubscriptionId id = impl_->next_id++;
    std::type_index ti{type};
    impl_->by_type[ti].push_back({id, std::move(handler)});
    impl_->id_to_type.emplace(id, ti);
    return id;
}

void Events::publish_erased(const std::type_info& type, const void* event) {
    // Snapshot the subscriber list under the lock so handlers can re-enter
    // subscribe/unsubscribe without deadlocking or having the iterator
    // invalidated mid-dispatch.
    std::vector<std::function<void(const void*)>> snapshot;
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        auto it = impl_->by_type.find(std::type_index{type});
        if (it == impl_->by_type.end()) return;
        snapshot.reserve(it->second.size());
        for (const auto& sub : it->second) snapshot.push_back(sub.handler);
    }
    for (auto& h : snapshot) h(event);
}

void Events::unsubscribe(SubscriptionId id) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    auto it = impl_->id_to_type.find(id);
    if (it == impl_->id_to_type.end()) return;
    auto& vec = impl_->by_type[it->second];
    vec.erase(std::remove_if(vec.begin(), vec.end(),
                             [id](const Impl::Sub& s) { return s.id == id; }),
              vec.end());
    impl_->id_to_type.erase(it);
}

} // namespace orca
