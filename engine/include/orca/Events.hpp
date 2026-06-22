#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <typeinfo>
#include <utility>

namespace orca {

using SubscriptionId = std::uint64_t;

// Typed pub/sub bus. `publish<E>` and `subscribe<E>` dispatch via std::type_info,
// so any event struct usable as a value type can be sent — no central registry
// is required. The Events bus is thread-safe; subscribers are called on the
// thread that publishes the event, and are responsible for any thread hop they
// need (e.g. wxQueueEvent from the GUI subscriber).
class Events {
public:
    ~Events();

    Events(const Events&)            = delete;
    Events& operator=(const Events&) = delete;
    Events(Events&&)                 = delete;
    Events& operator=(Events&&)      = delete;

    template <class E>
    SubscriptionId subscribe(std::function<void(const E&)> handler) {
        return subscribe_erased(
            typeid(E),
            [h = std::move(handler)](const void* p) { h(*static_cast<const E*>(p)); });
    }

    template <class E>
    void publish(const E& event) {
        publish_erased(typeid(E), static_cast<const void*>(&event));
    }

    void unsubscribe(SubscriptionId id);

private:
    friend class Session;
    Events();

    SubscriptionId subscribe_erased(const std::type_info& type, std::function<void(const void*)> handler);
    void           publish_erased(const std::type_info& type, const void* event);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace orca
