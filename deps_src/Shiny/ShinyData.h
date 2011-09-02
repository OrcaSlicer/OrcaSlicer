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

#ifndef SHINY_DATA_H
#define SHINY_DATA_H

#include "ShinyPrereqs.h"

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------*/

typedef struct {
	uint32_t entryCount;
	shinytick_t selfTicks;
} ShinyLastData;


/*---------------------------------------------------------------------------*/

typedef struct {
	shinytick_t cur;
	float avg;
} ShinyTickData;

typedef struct {
	uint32_t cur;
	float avg;
} ShinyCountData;

typedef struct {
	ShinyCountData entryCount;
	ShinyTickData selfTicks;
	ShinyTickData childTicks;
} ShinyData;

SHINY_INLINE shinytick_t ShinyData_totalTicksCur(const ShinyData *self) {
	return self->selfTicks.cur + self->childTicks.cur;
}

SHINY_INLINE float ShinyData_totalTicksAvg(const ShinyData *self) {
	return self->selfTicks.avg + self->childTicks.avg;
}

SHINY_INLINE void ShinyData_computeAverage(ShinyData *self, float a_damping) {
	self->entryCount.avg = self->entryCount.cur +
		a_damping * (self->entryCount.avg - self->entryCount.cur);
	self->selfTicks.avg = self->selfTicks.cur +
		a_damping * (self->selfTicks.avg - self->selfTicks.cur);
	self->childTicks.avg = self->childTicks.cur +
		a_damping * (self->childTicks.avg - self->childTicks.cur);
}

SHINY_INLINE void ShinyData_copyAverage(ShinyData *self) {
	self->entryCount.avg = (float) self->entryCount.cur;
	self->selfTicks.avg = (float) self->selfTicks.cur;
	self->childTicks.avg = (float) self->childTicks.cur;
}

SHINY_INLINE void ShinyData_clearAll(ShinyData *self) {
	self->entryCount.cur = 0;
	self->entryCount.avg = 0;
	self->selfTicks.cur = 0;
	self->selfTicks.avg = 0;
	self->childTicks.cur = 0;
	self->childTicks.avg = 0;
}

SHINY_INLINE void ShinyData_clearCurrent(ShinyData *self) {
	self->entryCount.cur = 0;
	self->selfTicks.cur = 0;
	self->childTicks.cur = 0;
}

#if __cplusplus
} /* end of extern "C" */
#endif

#endif /* SHINY_DATA_H */
