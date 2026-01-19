#ifndef MINILZO_EXTENSION_HPP
#define MINILZO_EXTENSION_HPP

#include <string>
#include <stdint.h>
#include "minilzo/minilzo.h"

namespace Slic3r {

int lzo_compress(unsigned char* in, uint64_t in_len, unsigned char* out, uint64_t* out_len);
int lzo_decompress(unsigned char* in, uint64_t in_len, unsigned char* out, uint64_t* out_len);


} // namespace Slic3r

#endif // MINIZ_EXTENSION_HPP
