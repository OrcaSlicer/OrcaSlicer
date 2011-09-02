/*
The MIT License

Copyright (c) 2007-2010 Aidin Abedi http://code.google.com/p/shinyprofiler/

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#ifdef SLIC3R_PROFILE

#include "ShinyTools.h"

#if SHINY_PLATFORM == SHINY_PLATFORM_WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
	#define NOMINMAX
#endif /* NOMINMAX */
#include <windows.h>

#elif SHINY_PLATFORM == SHINY_PLATFORM_POSIX
#include <sys/time.h>
#endif


/*---------------------------------------------------------------------------*/

const ShinyTimeUnit* ShinyGetTimeUnit(float ticks) {
	static ShinyTimeUnit units[4] = { 0 };

	if (units[0].tickFreq == 0) { /* auto initialize first time */
		units[0].tickFreq = ShinyGetTickFreq() / 1.0f;
		units[0].invTickFreq = ShinyGetTickInvFreq() * 1.0f;
		units[0].suffix = "s";

		units[1].tickFreq = ShinyGetTickFreq() / 1000.0f;
		units[1].invTickFreq = ShinyGetTickInvFreq() * 1000.0f;
		units[1].suffix = "ms";

		units[2].tickFreq = ShinyGetTickFreq() / 1000000.0f;
		units[2].invTickFreq = ShinyGetTickInvFreq() * 1000000.0f;
		units[2].suffix = "us";

		units[3].tickFreq = ShinyGetTickFreq() / 1000000000.0f;
		units[3].invTickFreq = ShinyGetTickInvFreq() * 1000000000.0f;
		units[3].suffix = "ns";
	}

	if (units[0].tickFreq < ticks) return &units[0];
	else if (units[1].tickFreq < ticks) return &units[1];
	else if (units[2].tickFreq < ticks) return &units[2];
	else return &units[3];
}


/*---------------------------------------------------------------------------*/

#if SHINY_PLATFORM == SHINY_PLATFORM_WIN32

void ShinyGetTicks(shinytick_t *p) {
	QueryPerformanceCounter((LARGE_INTEGER*)(p));
}

shinytick_t ShinyGetTickFreq(void) {
	static shinytick_t freq = 0;
	if (freq == 0) QueryPerformanceFrequency((LARGE_INTEGER*)(&freq));
	return freq;
}

float ShinyGetTickInvFreq(void) {
	static float invfreq = 0;
	if (invfreq == 0) invfreq = 1.0f / ShinyGetTickFreq();
	return invfreq;
}


/*---------------------------------------------------------------------------*/

#elif SHINY_PLATFORM == SHINY_PLATFORM_POSIX

//#include <time.h>
//#include <sys/time.h>

void ShinyGetTicks(shinytick_t *p) {
	struct timeval time;
	gettimeofday(&time, NULL);

	*p = time.tv_sec * 1000000 + time.tv_usec;
}

shinytick_t ShinyGetTickFreq(void) {
	return 1000000;
}

float ShinyGetTickInvFreq(void) {
	return 1.0f / 1000000.0f;
}

#endif

#endif /* SLIC3R_PROFILE */
