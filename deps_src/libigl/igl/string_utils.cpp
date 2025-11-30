// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2020 Jérémie Dumas <jeremie.dumas@ens-lyon.org>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "string_utils.h"

#include <cstring>

namespace igl {

IGL_INLINE bool starts_with(const std::string &str, const std::string &prefix) {
  return (str.rfind(prefix, 0) == 0);
}

IGL_INLINE bool starts_with(const char *str, const char* prefix) {
  return strncmp(prefix, str, strlen(prefix)) == 0;
}

}
