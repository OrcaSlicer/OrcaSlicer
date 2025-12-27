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

#ifndef SHINY_OUTPUT_H
#define SHINY_OUTPUT_H

#include "ShinyNode.h"
#include "ShinyZone.h"

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------*/

SHINY_API int ShinyPrintNodesSize(uint32_t a_count);
SHINY_API int ShinyPrintZonesSize(uint32_t a_count);

SHINY_API void ShinyPrintANode(char* output, const ShinyNode *a_node, const ShinyNode *a_root);
SHINY_API void ShinyPrintAZone(char* output, const ShinyZone *a_zone, const ShinyZone *a_root);

SHINY_API void ShinyPrintNodes(char* output, const ShinyNode *a_root);
SHINY_API void ShinyPrintZones(char* output, const ShinyZone *a_root);


/*---------------------------------------------------------------------------*/

#if __cplusplus
} /* end of extern "C" */
#include <string>

SHINY_INLINE std::string ShinyNodesToString(const ShinyNode *a_root, uint32_t a_count) {
	std::string str;
	str.resize(ShinyPrintNodesSize(a_count) - 1);
	ShinyPrintNodes(&str[0], a_root);
	return str;
}

SHINY_INLINE std::string ShinyZonesToString(const ShinyZone *a_root, uint32_t a_count) {
	std::string str;
	str.resize(ShinyPrintZonesSize(a_count) - 1);
	ShinyPrintZones(&str[0], a_root);
	return str;
}
#endif /* __cplusplus */

#endif /* SHINY_OUTPUT_H */
