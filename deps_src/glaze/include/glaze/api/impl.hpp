// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include "glaze/api/api.hpp"
#include "glaze/api/std/array.hpp"
#include "glaze/api/std/deque.hpp"
#include "glaze/api/std/functional.hpp"
#include "glaze/api/std/list.hpp"
#include "glaze/api/std/map.hpp"
#include "glaze/api/std/optional.hpp"
#include "glaze/api/std/shared_ptr.hpp"
#include "glaze/api/std/string.hpp"
#include "glaze/api/std/tuple.hpp"
#include "glaze/api/std/unique_ptr.hpp"
#include "glaze/api/std/unordered_map.hpp"
#include "glaze/api/std/variant.hpp"
#include "glaze/api/std/vector.hpp"
#include "glaze/api/tuplet.hpp"
#include "glaze/api/type_support.hpp"
#include "glaze/beve/read.hpp"
#include "glaze/beve/write.hpp"
#include "glaze/glaze.hpp"
#include "glaze/json/read.hpp"
#include "glaze/json/write.hpp"

namespace glz
{
   template <class UserType>
   struct impl : api
   {
      UserType user{};

      std::pair<void*, glz::hash_t> get(const sv path) noexcept override { return get_void(user, path); }

      [[nodiscard]] bool contains(const sv path) noexcept override
      {
         return seek([&](auto&&) {}, user, path);
      }

      bool read(const uint32_t format, const sv path, const sv data) noexcept override
      {
         error_ctx pe{};
         bool success;

         if (format == JSON) {
            success = seek([&](auto&& val) { pe = glz::read<opts{}>(val, data); }, user, path);
         }
         else {
            success = seek([&](auto&& val) { pe = glz::read<opts{.format = BEVE}>(val, data); }, user, path);
         }

         if (success) {
            if (pe) {
               return false;
            }
            return true;
         }
         return false;
      }

      bool write(const uint32_t format, const sv path, std::string& data) noexcept override
      {
         // TODO: Support write errors when seeking
         if (format == JSON) {
            return seek([&](auto&& val) { std::ignore = glz::write_json(val, data); }, user, path);
         }
         else {
            return seek([&](auto&& val) { std::ignore = glz::write_beve(val, data); }, user, path);
         }
      }

      std::unique_ptr<void, void (*)(void*)> get_fn(const sv path, const glz::hash_t type_hash) noexcept override
      {
         return get_void_fn(user, path, type_hash);
      }

      template <class T>
      using ref_t =
         typename std::conditional_t<!std::is_const_v<std::remove_reference_t<T>> && std::is_lvalue_reference_v<T>,
                                     std::add_lvalue_reference_t<std::decay_t<T>>,
                                     std::add_rvalue_reference_t<std::decay_t<T>>>;

      template <class T>
      ref_t<T> to_ref(void* t)
      {
         using V = ref_t<T>;
         return static_cast<V>(*reinterpret_cast<std::add_pointer_t<std::decay_t<T>>>(t));
      }

      template <class Arg_tuple, class F, class Parent, size_t... Is>
         requires std::invocable<F, Parent, ref_t<glz::tuple_element_t<Is, Arg_tuple>>...>
      decltype(auto) call_args(F&& f, Parent&& parent, [[maybe_unused]] std::span<void*> args,
                               std::index_sequence<Is...>)
      {
         return f(parent, to_ref<glz::tuple_element_t<Is, Arg_tuple>>(args[Is])...);
      }

      bool caller(const sv path, const glz::hash_t type_hash, void*& ret, std::span<void*> args) noexcept override
      {
         auto p = parent_last_json_ptrs(path);
         const auto parent_ptr = p.first;
         const auto last_ptr = p.second;

         bool found = false;

         seek(
            [&](auto&& parent) {
               using P = std::decay_t<decltype(parent)>;

               seek(
                  [&](auto&& val) {
                     using V = std::decay_t<decltype(val)>;
                     if constexpr (std::is_member_function_pointer_v<V>) {
                        using Parent = typename parent_of_fn<V>::type;

                        if constexpr (std::same_as<P, Parent>) {
                           using F = typename std_function_signature_decayed_keep_non_const_ref<V>::type;
                           static constexpr auto h = glz::hash<F>();
                           using Ret = typename return_type<V>::type;
                           using Tuple = typename inputs_as_tuple<V>::type;
                           static constexpr auto N = glz::tuple_size_v<Tuple>;

                           if (h == type_hash) [[likely]] {
                              if constexpr (std::is_void_v<Ret>) {
                                 call_args<Tuple>(std::mem_fn(val), parent, args, std::make_index_sequence<N>{});
                              }
                              else if constexpr (std::is_pointer_v<std::decay_t<Ret>>) {
                                 ret = call_args<Tuple>(std::mem_fn(val), parent, args, std::make_index_sequence<N>{});
                              }
                              else if constexpr (std::is_lvalue_reference_v<Ret>) {
                                 // TODO remove const cast
                                 ret = const_cast<std::decay_t<Ret>*>(
                                    &call_args<Tuple>(std::mem_fn(val), parent, args, std::make_index_sequence<N>{}));
                              }
                              else {
                                 *static_cast<Ret*>(ret) =
                                    call_args<Tuple>(std::mem_fn(val), parent, args, std::make_index_sequence<N>{});
                              }
                              found = true;
                           }
                           else [[unlikely]] {
                              error = "mismatching types";
                              error += ", expected: " + std::string(glz::name_v<F>);
                           }
                        }
                        else {
                           error = "invalid parent type";
                        }
                     }
                     else {
                        error = "caller: type is not a member function";
                     }
                  },
                  parent, last_ptr);
            },
            user, parent_ptr); // seek to parent

         if (found) {
            return true;
         }

         if (error.empty()) {
            error = "invalid path";
         }

         return false;
      }

     protected:
      template <class T>
      decltype(auto) unwrap(T&& val)
      {
         using V = std::decay_t<T>;
         if constexpr (nullable_t<V>) {
            if (val) {
               return unwrap(*val);
            }
            else {
               error = "Cannot unwrap null value.";
               return decltype(unwrap(*val)){nullptr};
            }
         }
         else {
            return &val;
         }
      }

      // Get a pointer to a value at the location of a json_ptr. Will return
      // nullptr if value doesnt exist or is wrong type
      template <class T>
      std::pair<void*, hash_t> get_void(T&& root_value, const sv json_ptr)
      {
         void* result{};

         glz::hash_t type_hash{};

         const auto success = seek(
            [&](auto&& val) {
               using V = std::decay_t<decltype(*unwrap(val))>;
               if constexpr (std::is_member_function_pointer_v<V>) {
                  error = "get called on member function pointer";
               }
               else {
                  static constexpr auto h = glz::hash<V>();
                  type_hash = h;
                  result = unwrap(val);
               }
            },
            std::forward<T>(root_value), json_ptr);

         if (!error.empty()) {
            return {nullptr, {}};
         }
         else if (!success) {
            error = "invalid path";
            return {nullptr, {}};
         }

         return {result, type_hash};
      }

      template <class T>
      auto get_void_fn(T& root_value, const sv json_ptr, const glz::hash_t type_hash)
      {
         std::unique_ptr<void, void (*)(void*)> result{nullptr, nullptr};

         auto p = parent_last_json_ptrs(json_ptr);
         const auto parent_ptr = p.first;
         const auto last_ptr = p.second;

         seek(
            [&](auto&& parent) {
               using P = std::decay_t<decltype(parent)>;

               seek(
                  [&](auto&& val) {
                     using V = std::decay_t<decltype(val)>;
                     if constexpr (std::is_member_function_pointer_v<V>) {
                        using Parent = typename parent_of_fn<V>::type;

                        if constexpr (std::same_as<P, Parent>) {
                           using F = typename std_function_signature<V>::type;
                           static constexpr auto h = glz::hash<F>();
                           if (h == type_hash) [[likely]] {
                              auto* f = new F{};
                              *f = [=, parent = &parent](auto&&... args) -> decltype(auto) {
                                 return ((*parent).*(val))(std::forward<decltype(args)>(args)...);
                              };
                              result = std::unique_ptr<void, void (*)(void*)>{
                                 f, [](void* ptr) { delete static_cast<F*>(ptr); }};
                           }
                           else [[unlikely]] {
                              error = "mismatching types";
                              error += ", expected: " + std::string(glz::name_v<F>);
                           }
                        }
                        else {
                           error = "invalid parent type";
                        }
                     }
                     else if constexpr (is_specialization_v<V, std::function>) {
                        static constexpr auto h = glz::hash<V>();
                        if (h == type_hash) [[likely]] {
                           result = std::unique_ptr<void, void (*)(void*)>{&val, [](void*) {}};
                        }
                        else [[unlikely]] {
                           error = "mismatching types";
                           error += ", expected: " + std::string(glz::name_v<V>);
                        }
                     }
                     else {
                        error =
                           "get_fn: type" + std::string(glz::name_v<V>) + " is not a member function or std::function";
                     }
                  },
                  parent, last_ptr);
            },
            root_value, parent_ptr); // seek to parent

         if (error.empty() && result == nullptr) {
            error = "invalid path";
         }

         return result;
      }
   };

   template <class T>
   inline constexpr auto make_api()
   {
      return std::shared_ptr<impl<T>>{new impl<T>{}, [](impl<T>* ptr) { delete ptr; }};
   }

   template <class... Args>
   iface_fn make_iface()
   {
      return [] {
         std::shared_ptr<iface> ptr{new iface{}, [](auto* ptr) { delete ptr; }};

         using T = std::tuple<Args...>;

         constexpr auto N = sizeof...(Args);
         for_each<N>([&]<auto I>() {
            using V = glz::tuple_element_t<I, T>;
            ptr->emplace(name_v<V>, make_api<V>);
         });

         return ptr;
      };
   }
}
