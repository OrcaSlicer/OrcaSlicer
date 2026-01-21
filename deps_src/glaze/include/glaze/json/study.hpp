// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <numeric>
#include <random>

#include "glaze/core/common.hpp"
#include "glaze/json/json_ptr.hpp"
#include "glaze/json/read.hpp"
#include "glaze/json/write.hpp"
#include "glaze/thread/threadpool.hpp"
#include "glaze/util/expected.hpp"
#include "glaze/util/type_traits.hpp"

namespace glz
{
   using basic = std::variant<bool, char, char8_t, unsigned char, signed char, short, unsigned short, float, int,
                              unsigned int, long, unsigned long, double, long long, unsigned long long, std::string>;

   // Explicitly defining basic_ptr to avoid additional template instantiations
   using basic_ptr =
      std::variant<bool*, char*, char8_t*, unsigned char*, signed char*, short*, unsigned short*, float*, int*,
                   unsigned int*, long*, unsigned long*, double*, long long*, unsigned long long*, std::string*>;

   namespace study
   {
      struct param
      {
         std::string ptr{};
         std::string distribution{}; // TODO: make an enum
         std::vector<raw_json> range{};
      };

      struct design
      {
         std::vector<param> params{}; //!< study parameters
         std::vector<std::unordered_map<std::string, raw_json>>
            states{}; //!< map of pointer syntax and json representation
         std::unordered_map<std::string, raw_json> overwrite{}; //!< pointer syntax and json representation
         std::default_random_engine::result_type seed{}; //!< Seed for randomized study
         size_t random_samples{}; //!< Number of runs to perform in randomized study.
                                  //! If zero it will run a full
                                  //!< factorial ignoring random distributions
                                  //!< instead instead of a randomized study
      };
   } // namespace study

   template <>
   struct meta<study::param>
   {
      using T = study::param;
      static constexpr auto value = object("ptr", &T::ptr, "dist", &T::distribution, "values", &T::range);
   };

   template <>
   struct meta<study::design>
   {
      using T = study::design;
      static constexpr auto value = object("params", &T::params, "states", &T::states, "overwrite", &T::overwrite,
                                           "seed", &T::seed, "random_samples", &T::random_samples);
   };

   namespace study
   {
      void overwrite(auto& state, const std::unordered_map<std::string, raw_json>& overwrites)
      {
         for (auto&& [json_ptr, raw_json_str] : overwrites) {
            read_as_json(state, json_ptr, raw_json_str.str);
         }
      }

      struct param_set
      {
         basic_ptr param_ptr{}; // to only seek once
         std::vector<basic> elements{};
      };

      template <class State>
      struct full_factorial
      {
         State state{};
         std::vector<param_set> param_sets;
         std::size_t index{};
         std::size_t max_index{};

         full_factorial(State _state, const design& design) : state(std::move(_state))
         {
            max_index = design.params.empty() ? 0 : 1;

            overwrite(state, design.overwrite);

            for (auto& param : design.params) {
               const auto pe = param_set_from_dist(param);
               // TODO: handle potential errors
               auto& ps = param_sets.emplace_back(*pe);
               const auto num_elements = ps.elements.size();
               if (num_elements != 0) {
                  max_index *= num_elements;
               }
            }
         }

         bool done() const { return index >= max_index; }

         std::size_t size() const { return max_index; }

         [[nodiscard]] expected<State, error_code> generate(const size_t i)
         {
            size_t deconst_index = i;
            for (auto& param_set : param_sets) {
               const auto this_size = (std::max)(param_set.elements.size(), size_t{1});
               const auto this_index = deconst_index % this_size;
               deconst_index /= this_size;
               const auto ec = std::visit(
                  [&](auto&& param_ptr) {
                     using V = std::remove_pointer_t<std::decay_t<decltype(param_ptr)>>;

                     auto& element = param_set.elements[this_index];
                     if (std::holds_alternative<double>(element)) {
                        if constexpr (std::is_convertible_v<V, double>) {
                           *param_ptr = static_cast<V>(std::get<double>(element));
                        }
                        else {
                           return error_code::elements_not_convertible_to_design;
                        }
                     }
                     else {
                        *param_ptr = std::get<V>(param_set.elements[this_index]);
                     }

                     return error_code::none;
                  },
                  param_set.param_ptr);

               if (bool(ec)) {
                  return unexpected(ec);
               }
            }

            return state;
         }

         [[nodiscard]] expected<State, error_code> generate() { return generate(index++); }

         expected<param_set, error_ctx> param_set_from_dist(const param& dist)
         {
            param_set param_set;

            error_ctx pe{};
            bool found = seek(
               [&](auto&& val) {
                  if constexpr (std::is_assignable_v<basic, std::decay_t<decltype(val)>>) {
                     param_set.param_ptr = &val;
                  }
                  else {
                     pe.ec = error_code::no_matching_variant_type; // Study params only support basic types like
                                                                   // double, int, bool, or std::string
                  }
               },
               state, dist.ptr);
            if (!found) {
               pe.ec = error_code::nonexistent_json_ptr;
               return unexpected(pe);
            }

            if (pe) {
               return unexpected(pe);
            }

            if (dist.distribution == "elements") {
               std::visit(
                  [&](auto&& param_ptr) {
                     for (auto&& j : dist.range) {
                        auto elem = *param_ptr;
                        pe = read_json(elem, j.str);
                        if (pe) {
                           return;
                        }
                        param_set.elements.emplace_back(elem);
                     }
                  },
                  param_set.param_ptr);
               if (pe) {
                  return unexpected(pe);
               }
            }
            else if (dist.distribution == "linspace") {
               if (dist.range.size() != 3) {
                  return unexpected(error_ctx{
                     0, error_code::invalid_distribution_elements}); // distribution's range must have 3 elements
               }

               double start{};
               double step{};
               double stop{};

               if (auto ec = read_json(start, dist.range[0].str)) {
                  return unexpected(ec);
               }
               if (auto ec = read_json(step, dist.range[1].str)) {
                  return unexpected(ec);
               }
               if (auto ec = read_json(stop, dist.range[2].str)) {
                  return unexpected(ec);
               }

               if (start > stop) {
                  std::swap(start, stop);
               }

               for (; start <= stop; start += step) {
                  param_set.elements.emplace_back(start);
               }
            }
            else {
               return unexpected(error_ctx{0, error_code::unknown_distribution});
            }

            return param_set;
         }
      };

      template <class T>
      concept generator = requires(T g) {
         g.generate();
         g.done();
      };

      // Takes a state generator and a function on which to invoke the state
      void run_study(generator auto& g, auto&& f)
      {
         glz::pool pool{};
         size_t job_num = 0;
         while (!g.done()) {
            // generate mutates
            // TODO: maybe save states and mutate them across threads
            pool.emplace_back([=, state = g.generate()](const auto) { f(std::move(state), job_num); });
            ++job_num;
         }
         pool.wait();
      }

      template <class T>
         requires range<T>
      void run_study(T& states, auto&& f)
      {
         glz::pool pool{};
         const auto n = states.size();
         for (size_t i = 0; i < n; ++i) {
            pool.emplace_back([=, state = states[i]](const auto) { f(std::move(state), i); });
         }
         pool.wait();
      }

      struct random_param
      {
         basic_ptr param_ptr{};
         basic value{};
         std::function<basic()> gen{};
         void apply()
         {
            std::visit([&](auto&& p) { *p = *std::get_if<std::remove_pointer_t<std::decay_t<decltype(p)>>>(&value); },
                       param_ptr);
         }
      };

      template <class State>
      struct random_doe
      {
         State state{};

         std::default_random_engine::result_type seed{};
         size_t random_samples{};

         std::default_random_engine engine{};
         std::vector<size_t> resample_indices{};
         size_t index = 0;

         std::vector<std::vector<random_param>> params_per_state{};

         random_doe(State _state, const design& design) : state(std::move(_state))
         {
            overwrite(state, design.overwrite);

            engine.seed(seed);
            resample_indices.resize(random_samples);
            std::iota(std::begin(resample_indices), std::end(resample_indices), 0);

            params_per_state.resize(random_samples);
            const size_t dim = design.params.size();

            if (params_per_state.size() > 0) {
               auto& params = params_per_state.front();
               params.resize(dim);
               for (std::size_t i = 0; i < dim; i++) {
                  // TODO: Fix this. It is unsafe to derefrence this but we cant return an error code.
                  params[i] = *param_from_dist(design.params[i]);
               }
            }

            resample(1.0);
         }

         bool done() const { return index >= params_per_state.size(); }

         const State& generate(const size_t i)
         {
            auto& params = params_per_state[i];
            for (auto& param : params) {
               param.apply();
            }

            return state;
         }

         const State& generate() { return generate(index++); }

         void reset() { index = 0; }

         std::size_t size() { return params_per_state.size(); }

         void resample(double ratio)
         {
            std::shuffle(std::begin(resample_indices), std::end(resample_indices), engine);
            std::size_t to_resample = static_cast<std::size_t>(std::ceil(ratio * params_per_state.size()));

            for (std::size_t i = 0; i < to_resample; ++i) {
               auto& params = params_per_state[resample_indices[i]];
               for (auto& param : params) {
                  param.value = param.gen();
               }
            }

            reset();
         }

         expected<random_param, error_ctx> param_from_dist(const param& dist)
         {
            random_param result{};

            error_ctx pe{};
            bool found = seek(
               [&](auto&& val) {
                  if constexpr (std::is_assignable_v<basic, std::decay_t<decltype(val)>>) {
                     result.param_ptr = &val;
                  }
                  else {
                     pe.ec = error_code::no_matching_variant_type; // Study params only support basic types like
                                                                   // double, int, bool, or std::string
                  }
               },
               state, dist.ptr);
            if (!found) {
               pe.ec = error_code::nonexistent_json_ptr;
               return unexpected(pe);
            }

            if (pe) {
               return unexpected(pe);
            }

            if (dist.distribution == "elements") {
               if (dist.range.size() == 0) {
                  return unexpected(error_ctx{0, error_code::invalid_distribution_elements});
               }
               std::vector<basic> elements{};
               std::visit(
                  [&](auto&& param_ptr) {
                     for (auto&& json : dist.range) {
                        auto elem = *param_ptr;
                        read_json(elem, json.str);
                        elements.emplace_back(elem);
                     }
                  },
                  result.param_ptr);
               result.gen = [this, dist = std::uniform_int_distribution<std::size_t>(0, dist.range.size() - 1),
                             elements = std::move(elements)]() mutable {
                  std::size_t element_index = dist(this->engine);
                  return elements[element_index];
               };
            }
            else if (dist.distribution == "linspace") {
               if (dist.range.size() != 3) {
                  return unexpected(error_ctx{
                     0, error_code::invalid_distribution_elements}); // distribution's range must have 3 elements
               }

               double start{};
               double stop{};

               if (auto ec = read_json(start, dist.range[0].str)) {
                  return unexpected(ec);
               }
               if (auto ec = read_json(stop, dist.range[2].str)) {
                  return unexpected(ec);
               }

               if (start > stop) {
                  std::swap(start, stop);
               }

               result.gen = [this, dist = std::uniform_real_distribution<double>(start, stop)]() mutable {
                  return dist(this->engine);
               };
            }
            else if (dist.distribution == "uniform") {
               if (dist.range.size() != 2) {
                  return unexpected(error_ctx{
                     0, error_code::invalid_distribution_elements}); // distribution's range must have 3 elements
               }

               double start{};
               double stop{};

               if (auto ec = read_json(start, dist.range[0].str)) {
                  return unexpected(ec);
               }
               if (auto ec = read_json(stop, dist.range[1].str)) {
                  return unexpected(ec);
               }

               if (start > stop) {
                  std::swap(start, stop);
               }

               result.gen = [this, dist = std::uniform_real_distribution<double>(start, stop)]() mutable {
                  return dist(this->engine);
               };
            }
            else if (dist.distribution == "normal") {
               if (dist.range.size() != 2) {
                  return unexpected(error_ctx{
                     0, error_code::invalid_distribution_elements}); // distribution's range must have 3 elements
               }

               double mean{};
               double std_dev{};

               if (auto ec = read_json(mean, dist.range[0].str)) {
                  return unexpected(ec);
               }
               if (auto ec = read_json(std_dev, dist.range[1].str)) {
                  return unexpected(ec);
               }

               result.gen = [this, dist = std::normal_distribution<double>(mean, std_dev)]() mutable {
                  return dist(this->engine);
               };
            }
            else {
               return unexpected(error_ctx{0, error_code::unknown_distribution});
            }
         }
      };
   }
}
