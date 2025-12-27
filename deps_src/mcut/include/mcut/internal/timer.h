/**
 * Copyright (c) 2021-2022 Floyd M. Chitalu.
 * All rights reserved.
 *
 * NOTE: This file is licensed under GPL-3.0-or-later (default).
 * A commercial license can be purchased from Floyd M. Chitalu.
 *
 * License details:
 *
 * (A)  GNU General Public License ("GPL"); a copy of which you should have
 *      recieved with this file.
 * 	    - see also: <http://www.gnu.org/licenses/>
 * (B)  Commercial license.
 *      - email: floyd.m.chitalu@gmail.com
 *
 * The commercial license options is for users that wish to use MCUT in
 * their products for comercial purposes but do not wish to release their
 * software products under the GPL license.
 *
 * Author(s)     : Floyd M. Chitalu
 */
#ifndef MCUT_TIMER_H_
#define MCUT_TIMER_H_

#include <chrono>
#include <map>
#include <memory>
#include <stack>
#include <sstream>

#include "mcut/internal/utils.h"
#include "mcut/internal/tpool.h"

//#define PROFILING_BUILD

class mini_timer {
    std::chrono::time_point<std::chrono::steady_clock> m_start;
    const std::string m_name;
    bool m_valid = true;

public:
    mini_timer(const std::string& name)
        : m_start(std::chrono::steady_clock::now())
        , m_name(name)
    {
    }

    ~mini_timer()
    {
        if (m_valid) {
            const std::chrono::time_point<std::chrono::steady_clock> now = std::chrono::steady_clock::now();
            const std::chrono::milliseconds elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_start);
            unsigned long long elapsed_ = elapsed.count();
            log_msg("[MCUT][PROF:" << std::this_thread::get_id() << "]: \"" << m_name << "\" ("<< elapsed_ << "ms)");
        }
    }
    void set_invalid()
    {
        m_valid = false;
    }
};

extern thread_local std::stack<std::unique_ptr<mini_timer>> g_thrd_loc_timerstack;

#if defined(PROFILING_BUILD)

#define TIMESTACK_PUSH(name) \
    g_thrd_loc_timerstack.push(std::unique_ptr<mini_timer>(new mini_timer(name)))
#define TIMESTACK_POP() \
    g_thrd_loc_timerstack.pop()
#define TIMESTACK_RESET()                                              \
    while (!g_thrd_loc_timerstack.empty()) {        \
        g_thrd_loc_timerstack.top()->set_invalid(); \
        g_thrd_loc_timerstack.pop();                \
    }
#define SCOPED_TIMER(name) \
    mini_timer _1mt(name)
#else
#define SCOPED_TIMER(name)
#define TIMESTACK_PUSH(name)
#define TIMESTACK_POP()
#define TIMESTACK_RESET()
#endif



#endif