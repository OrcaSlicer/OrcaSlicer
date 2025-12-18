
//              Copyright Catch2 Authors
// Distributed under the Boost Software License, Version 1.0.
//   (See accompanying file LICENSE.txt or copy at
//        https://www.boost.org/LICENSE_1_0.txt)

// SPDX-License-Identifier: BSL-1.0
#include <catch2/internal/catch_context.hpp>
#include <catch2/internal/catch_random_number_generator.hpp>

namespace Catch {

    Context Context::currentContext;

    Context& getCurrentMutableContext() {
        return Context::currentContext;
    }

    SimplePcg32& sharedRng() {
        static SimplePcg32 s_rng;
        return s_rng;
    }

}
