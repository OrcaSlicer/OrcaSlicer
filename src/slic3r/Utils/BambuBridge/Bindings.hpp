#pragma once

#include "Rpc.hpp"
#include "JsonTypes.hpp"
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace Slic3r::BambuBridge {

// Callback IDs refer to functions in the client, never code addresses in the DLL process.
class Callbacks {
public:
    using Function = std::function<Json(const Json&)>;
    std::uint64_t add(Function fn)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto id = ++m_next_id;
        m_functions.emplace(id, std::move(fn));
        return id;
    }
    void erase(const std::vector<std::uint64_t>& ids)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto id : ids) m_functions.erase(id);
    }
    Json invoke(const Json& args)
    {
        Function fn;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            fn = m_functions.at(args.at("callback").get<std::uint64_t>());
        }
        return fn(args.at("args"));
    }
private:
    std::mutex m_mutex;
    std::uint64_t m_next_id = 0;
    std::map<std::uint64_t, Function> m_functions;
};

// Native-side stand-in for the one helper-owned agent. It is never dereferenced or sent.
inline void* agent_token() { static unsigned char token; return &token; }

template<class T> struct ClientValue {
    static Json encode(const T& value, Callbacks&, std::vector<std::uint64_t>&) { return value; }
};
template<> struct ClientValue<void*> {
    static Json encode(void* value, Callbacks&, std::vector<std::uint64_t>&)
    {
        if (value && value != agent_token()) throw std::runtime_error("Invalid remote Bambu agent");
        return value ? 1 : 0;
    }
};
template<class T> struct ClientValue<T*> {
    static Json encode(T* value, Callbacks&, std::vector<std::uint64_t>&) { return value ? Json(*value) : Json(); }
};

template<class R, class... A> struct ClientValue<std::function<R(A...)>> {
    template<std::size_t... I>
    static Json invoke(const std::function<R(A...)>& fn, const Json& args, std::index_sequence<I...>)
    {
        if (args.size() != sizeof...(A)) throw std::runtime_error("Invalid callback argument count");
        if constexpr (std::is_void_v<R>) { fn(args.at(I).template get<std::decay_t<A>>()...); return nullptr; }
        else return fn(args.at(I).template get<std::decay_t<A>>()...);
    }
    static Json encode(std::function<R(A...)> fn, Callbacks& callbacks, std::vector<std::uint64_t>& ids)
    {
        if (!fn) return nullptr;
        const auto id = callbacks.add([fn = std::move(fn)](const Json& args) {
            return invoke(fn, args, std::index_sequence_for<A...>{});
        });
        ids.push_back(id);
        return id;
    }
};

template<class T> struct ServerValue {
    using Value = std::decay_t<T>;
    Value value;
    ServerValue(const Json& input, Rpc&, void*) : value(input.template get<Value>()) {}
    decltype(auto) get() { return (value); }
    Json output() const { return value; }
};
template<> struct ServerValue<void*> {
    void* value;
    ServerValue(const Json& input, Rpc&, void* agent) : value(agent)
    {
        if (input != 1 || !agent) throw std::runtime_error("Invalid remote Bambu agent");
    }
    void* get() { return value; }
    Json output() const { return 1; }
};
template<class T> struct ServerValue<T*> {
    T value{};
    bool present;
    ServerValue(const Json& input, Rpc&, void*) : present(!input.is_null())
    { if (present) value = input.template get<T>(); }
    T* get() { return present ? &value : nullptr; }
    Json output() const { return present ? Json(value) : Json(); }
};
template<class R, class... A> struct ServerValue<std::function<R(A...)>> {
    std::function<R(A...)> value;
    ServerValue(const Json& input, Rpc& rpc, void*)
    {
        if (input.is_null()) return;
        const auto id = input.get<std::uint64_t>();
        value = [&rpc, id](A... args) -> R {
            try {
                auto result = rpc.request("callback", {{"callback", id}, {"args", Json::array({args...})}});
                if constexpr (!std::is_void_v<R>) return result.template get<R>();
            } catch (...) {
                // Stop jobs when the client disappears. No DLL worker may throw across its ABI.
                if constexpr (std::is_same_v<R, bool>) return sizeof...(A) == 0; // cancel=true, wait=false
                else if constexpr (!std::is_void_v<R>) return R{};
            }
        };
    }
    auto& get() { return value; }
    Json output() const { return nullptr; }
};

template<class F> struct Binding;
template<class R, class... A> struct Binding<R (*)(A...)> {
    using Function = R (*)(A...);
    template<std::size_t... I>
    static Json serve(Function fn, const Json& input, Rpc& rpc, void* agent, std::index_sequence<I...>)
    {
        if (!input.is_array() || input.size() != sizeof...(A)) throw std::runtime_error("Invalid Bambu argument count");
        std::tuple<ServerValue<A>...> values{ServerValue<A>(input.at(I), rpc, agent)...};
        Json result;
        if constexpr (std::is_void_v<R>) fn(std::get<I>(values).get()...);
        else if constexpr (std::is_same_v<R, void*>) result = fn(std::get<I>(values).get()...) ? 1 : 0;
        else result = fn(std::get<I>(values).get()...);
        return {{"value", result}, {"args", Json::array({std::get<I>(values).output()...})}};
    }
    static Json serve(Function fn, const Json& input, Rpc& rpc, void* agent)
    { return serve(fn, input, rpc, agent, std::index_sequence_for<A...>{}); }

    template<class T> static void copy_out(T&& arg, const Json& output)
    {
        using Value = std::remove_reference_t<T>;
        if constexpr (std::is_pointer_v<Value> && !std::is_same_v<Value, void*>) {
            if (arg && !output.is_null()) *arg = output.template get<std::remove_pointer_t<Value>>();
        } else if constexpr (std::is_lvalue_reference_v<T> && !std::is_const_v<Value>) {
            arg = output.template get<Value>();
        }
    }
    template<std::size_t... I>
    static void outputs(std::tuple<A...>& args, const Json& output, std::index_sequence<I...>)
    {
        // Only reference and pointer parameters are outputs. By-value callbacks are not.
        (copy_argument<A>(std::get<I>(args), output.at(I)), ...);
    }
    template<class Declared, class T> static void copy_argument(T& arg, const Json& output)
    {
        if constexpr (!std::is_same_v<std::decay_t<Declared>, void*> &&
                      (std::is_reference_v<Declared> || std::is_pointer_v<Declared>)) copy_out(arg, output);
    }
    static R call(Rpc& rpc, Callbacks& callbacks, const std::string& method,
                  std::vector<std::uint64_t>& ids, A... args)
    {
        auto input = Json::array({ClientValue<std::decay_t<A>>::encode(args, callbacks, ids)...});
        const bool long_job = method.find("bambu_network_start_") == 0 &&
                              (method.find("print") != std::string::npos || method.find("gcode") != std::string::npos);
        auto result = rpc.request(method, std::move(input), long_job ? std::chrono::hours(24) :
                                  method == "bambu_network_bind" ? std::chrono::minutes(5) : std::chrono::seconds(30));
        std::tuple<A...> tuple(args...);
        outputs(tuple, result.at("args"), std::index_sequence_for<A...>{});
        if constexpr (std::is_same_v<R, void*>) return result.at("value") == 1 ? agent_token() : nullptr;
        else if constexpr (!std::is_void_v<R>) return result.at("value").template get<R>();
    }
};

} // namespace Slic3r::BambuBridge
