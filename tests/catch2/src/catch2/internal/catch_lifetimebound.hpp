
//              Copyright Catch2 Authors
// Distributed under the Boost Software License, Version 1.0.
//   (See accompanying file LICENSE.txt or copy at
//        https://www.boost.org/LICENSE_1_0.txt)

// SPDX-License-Identifier: BSL-1.0

#ifndef CATCH_LIFETIMEBOUND_HPP_INCLUDED
#define CATCH_LIFETIMEBOUND_HPP_INCLUDED

#if !defined( __has_cpp_attribute )
#    define CATCH_ATTR_LIFETIMEBOUND
#elif __has_cpp_attribute( msvc::lifetimebound )
#    define CATCH_ATTR_LIFETIMEBOUND [[msvc::lifetimebound]]
#elif __has_cpp_attribute( clang::lifetimebound )
#    define CATCH_ATTR_LIFETIMEBOUND [[clang::lifetimebound]]
#elif __has_cpp_attribute( lifetimebound )
#    define CATCH_ATTR_LIFETIMEBOUND [[lifetimebound]]
#else
#    define CATCH_ATTR_LIFETIMEBOUND
#endif

#endif // CATCH_LIFETIMEBOUND_HPP_INCLUDED
