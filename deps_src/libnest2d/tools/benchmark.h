/*
 * Copyright (C) Tamás Mészáros
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */
#ifndef INCLUDE_BENCHMARK_H_
#define INCLUDE_BENCHMARK_H_

#include <chrono>
#include <ratio>

/**
 * A class for doing benchmarks.
 */
class Benchmark {
    typedef std::chrono::high_resolution_clock Clock;
    typedef Clock::duration Duration;
    typedef Clock::time_point TimePoint;

    TimePoint t1, t2;
    Duration d;

    inline double to_sec(Duration d) {
        return d.count() * double(Duration::period::num) / Duration::period::den;
    }

public:

    /**
     *  Measure time from the moment of this call.
     */
    void start() {  t1 = Clock::now(); }

    /**
     *  Measure time to the moment of this call.
     */
    void stop() {  t2 = Clock::now(); }

    /**
     * Get the time elapsed between a start() end a stop() call.
     * @return Returns the elapsed time in seconds.
     */
    double getElapsedSec() {  d = t2 - t1; return to_sec(d); }
};


#endif /* INCLUDE_BENCHMARK_H_ */
