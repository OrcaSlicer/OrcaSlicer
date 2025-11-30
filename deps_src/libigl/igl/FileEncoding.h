// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2020 Jérémie Dumas <jeremie.dumas@ens-lyon.org>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_FILEENCODING_H
#define IGL_FILEENCODING_H

namespace igl
{
  /// File encoding types for writing files.
enum class FileEncoding {
  Binary,
  Ascii
};

}

#endif
