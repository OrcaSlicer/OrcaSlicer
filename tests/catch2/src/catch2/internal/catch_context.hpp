
//              Copyright Catch2 Authors
// Distributed under the Boost Software License, Version 1.0.
//   (See accompanying file LICENSE.txt or copy at
//        https://www.boost.org/LICENSE_1_0.txt)

// SPDX-License-Identifier: BSL-1.0
#ifndef CATCH_CONTEXT_HPP_INCLUDED
#define CATCH_CONTEXT_HPP_INCLUDED

#include <catch2/internal/catch_compiler_capabilities.hpp>

namespace Catch {

    class IResultCapture;
    class IConfig;

    class Context {
        IConfig const* m_config = nullptr;
        IResultCapture* m_resultCapture = nullptr;

        CATCH_EXPORT static Context currentContext;
        friend Context& getCurrentMutableContext();
        friend Context const& getCurrentContext();

    public:
        constexpr IResultCapture* getResultCapture() const {
            return m_resultCapture;
        }
        constexpr IConfig const* getConfig() const { return m_config; }
        constexpr void setResultCapture( IResultCapture* resultCapture ) {
            m_resultCapture = resultCapture;
        }
        constexpr void setConfig( IConfig const* config ) { m_config = config; }
    };

    Context& getCurrentMutableContext();

    inline Context const& getCurrentContext() {
        return Context::currentContext;
    }

    class SimplePcg32;
    SimplePcg32& sharedRng();
}

#endif // CATCH_CONTEXT_HPP_INCLUDED
