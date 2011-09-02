#include <exception>
#include <stdio.h>
#include <stdlib.h>
#include "minilzo_extension.hpp"
#include "minilzo/minilzo.h"

static unsigned char wrkmem[LZO1X_1_MEM_COMPRESS];

static bool initialized = false;

namespace Slic3r {

int lzo_compress(unsigned char *in, uint64_t in_len, unsigned char* out, uint64_t* out_len)
{
	int result = 0;
	if (!initialized) {
		if (lzo_init() != LZO_E_OK)
			return LZO_E_ERROR;
		else
			initialized = true;
	}

	lzo_uint lzo_out_len = *out_len;
	result = lzo1x_1_compress(in, in_len, out, &lzo_out_len, wrkmem);
	if (result == LZO_E_OK) {
		*out_len = lzo_out_len;
		return 0;
	}
	
	return result;
}


int lzo_decompress(unsigned char *in, uint64_t in_len, unsigned char* out, uint64_t* out_len)
{
	int result = 0;
	if (!initialized) {
		if (lzo_init() != LZO_E_OK)
			return LZO_E_ERROR;
		else
			initialized = true;
	}

	lzo_uint lzo_out_len = *out_len;
	result = lzo1x_decompress(in, in_len, out, &lzo_out_len, NULL);
	if (result == LZO_E_OK) {
		*out_len = lzo_out_len;
		return 0;
	}
	
	return result;
}

} // namespace Slic3r