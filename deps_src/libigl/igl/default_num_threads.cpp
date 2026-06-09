// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2021 Jérémie Dumas <jeremie.dumas@ens-lyon.org>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#include "default_num_threads.h"

#include <cstdlib>
#include <thread>

IGL_INLINE unsigned int igl::default_num_threads(unsigned int user_num_threads) {
  // Thread-safe initialization using Meyers' singleton
  class MySingleton {
  public:
    static MySingleton &instance(unsigned int force_num_threads) {
      static MySingleton instance(force_num_threads);
      return instance;
    }

    unsigned int get_num_threads() const { return m_num_threads; }

  private:
    static const char* getenv_nowarning(const char* env_var)
    {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
      return std::getenv(env_var);
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    }

    MySingleton(unsigned int force_num_threads) {
      // User-defined default
      if (force_num_threads) {
        m_num_threads = force_num_threads;
        return;
      }
      // Set from env var
      if (const char *env_str = getenv_nowarning("IGL_NUM_THREADS")) {
        const int env_num_thread = atoi(env_str);
        if (env_num_thread > 0) {
          m_num_threads = static_cast<unsigned int>(env_num_thread);
          return;
        }
      }
      // Guess from hardware
      const unsigned int hw_num_threads = std::thread::hardware_concurrency();
      if (hw_num_threads) {
        m_num_threads = hw_num_threads;
        return;
      }
      // Fallback when std::thread::hardware_concurrency doesn't work
      m_num_threads = 8u;
    }

    unsigned int m_num_threads = 0;
  };

  return MySingleton::instance(user_num_threads).get_num_threads();
}
