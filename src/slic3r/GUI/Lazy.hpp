#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "StagedBuild.hpp"

class wxBusyCursor;

// Deferred construction of one object. Lazy<T> holds the factory and builds the object on
// demand or, unit by unit, from an IdleScheduler; LazyBase is what the scheduler sees of
// it; LazyInstance<Self> gives a type with one such object app-wide static access to it.
// docs/HLSD/deferred-page-construction.md covers the design.

namespace Slic3r { namespace GUI {

template <class Self> class LazyInstance;

// A prebuild task: what PrebuildQueue sees of a Lazy<T>. A type with its own unit sequence
// implements it directly.
class LazyBase
{
public:
    virtual ~LazyBase() = default;

    // Labels the object in logs.
    virtual const std::string& name() const = 0;
    virtual bool               built() const = 0;
    // Whether the idle prebuild has work here.
    virtual bool pending() const { return !built(); }
    // Runs one unit and returns true while more remain.
    virtual bool build_step() = 0;
    // Position in the idle queue: lower builds first, negative is never prebuilt.
    virtual int prebuild_order() const = 0;

protected:
    // Busy cursor around a build the user is waiting on, and a log line after it.
    class OnDemandBuild
    {
    public:
        explicit OnDemandBuild(const LazyBase& lazy);
        ~OnDemandBuild();
        void unit() { ++m_units; }

    private:
        const LazyBase&               m_lazy;
        std::unique_ptr<wxBusyCursor> m_busy;
        std::chrono::steady_clock::time_point m_started;
        int                           m_units{ 0 };
    };

    static void log_null_factory(const std::string& name);
};

// Holds an object a factory makes on the first build_step() or ensure(), followed by one
// StagedBuild step per unit if the type has them, and keeps callbacks until the object is
// complete. The object's parent owns it, not the holder. A LazyInstance type is registered
// for its statics; any other type is reached only through the holder.
template <class T>
class Lazy : public LazyBase
{
public:
    using Factory = std::function<T*()>;

    Lazy(std::string name, int order, Factory make) : m_name(std::move(name)), m_order(order), m_make(std::move(make))
    {
        if constexpr (registered())
            LazyInstance<T>::s_lazy = this;
    }
    ~Lazy() override
    {
        if constexpr (registered())
            if (LazyInstance<T>::s_lazy == this)
                LazyInstance<T>::s_lazy = nullptr;
    }
    Lazy(const Lazy&) = delete;
    Lazy& operator=(const Lazy&) = delete;

    // Null until completely built.
    T* get() const { return built() ? m_object : nullptr; }

    // Builds whatever is left now and returns the object, null if the factory returned null
    // or a nested call finds the object mid-build.
    T* ensure()
    {
        if (!built() && !m_building) {
            OnDemandBuild build(*this);
            do
                build.unit();
            while (build_step());
        }
        return get();
    }

    // Runs fn on the object now if it is built, otherwise once it is.
    void when_built(std::function<void(T&)> fn)
    {
        if (built())
            fn(*m_object);
        else
            m_deferred.push_back(std::move(fn));
    }

    const std::string& name() const override { return m_name; }
    // Read by a worker thread through the statics; the last write in build_step() releases it.
    bool built() const override { return m_complete.load(std::memory_order_acquire); }
    bool pending() const override { return !built() && !m_failed; }
    int  prebuild_order() const override { return m_order; }

    // The factory is the first unit, then one StagedBuild step each. A unit that pumps the
    // event loop cannot re-enter; a nested call does nothing.
    bool build_step() override
    {
        if (built() || m_building || m_failed)
            return false;
        m_building = true;
        try {
            if (m_object == nullptr)
                m_object = m_make();
            else
                step();
        } catch (...) {
            m_building = false;
            throw;
        }
        m_building = false;
        if (m_object == nullptr) {
            m_failed = true;
            log_null_factory(m_name);
            return false;
        }
        if (steps_remain())
            return true;
        m_complete.store(true, std::memory_order_release);
        for (auto& fn : m_deferred)
            fn(*m_object);
        m_deferred.clear();
        return false;
    }

private:
    static constexpr bool registered() { return std::is_base_of_v<LazyInstance<T>, T>; }
    static constexpr bool staged() { return std::is_base_of_v<StagedBuild, T>; }

    bool steps_remain() const
    {
        if constexpr (staged())
            return !m_object->built();
        else
            return false;
    }
    void step()
    {
        if constexpr (staged())
            m_object->build_step();
    }

    std::string                          m_name;
    int                                  m_order;
    Factory                              m_make;
    T*                                   m_object{ nullptr };
    std::atomic<bool>                    m_complete{ false };
    bool                                 m_building{ false };
    bool                                 m_failed{ false };
    std::vector<std::function<void(T&)>> m_deferred;
};

// Mixin for a type with one lazily built instance in the app. The statics reach that
// instance through its Lazy holder (a recreated MainFrame's holder replaces the old
// frame's).
template <class Self>
class LazyInstance
{
public:
    // Null until completely built.
    static Self* if_built() { return s_lazy ? s_lazy->get() : nullptr; }
    // Builds the object if needed; null while no holder exists or the holder has no
    // object.
    static Self* ensure() { return s_lazy ? s_lazy->ensure() : nullptr; }
    // Runs fn on the object now if it is built, otherwise once it is.
    static void when_built(std::function<void(Self&)> fn)
    {
        if (s_lazy)
            s_lazy->when_built(std::move(fn));
    }

private:
    friend class Lazy<Self>;
    inline static Lazy<Self>* s_lazy{ nullptr };
};

}} // namespace Slic3r::GUI
