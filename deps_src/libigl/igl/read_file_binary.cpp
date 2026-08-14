// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2020 Jérémie Dumas <jeremie.dumas@ens-lyon.org>
// Copyright (C) 2021 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "read_file_binary.h"

#include <cstring>
#include <cstdint>

IGL_INLINE void igl::read_file_binary(
  FILE *fp,
  std::vector<std::uint8_t> &fileBufferBytes) 
{
  if (!ferror(fp)) 
  {
    fseek(fp, 0, SEEK_END);
    size_t sizeBytes = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    fileBufferBytes.resize(sizeBytes);
    if(fread((char*)fileBufferBytes.data(), 1, sizeBytes, fp) == sizeBytes) 
    {
      fclose(fp);
      return;
    }
  }
  throw std::runtime_error("error reading from file");
}
