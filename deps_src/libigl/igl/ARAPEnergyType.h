// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_ARAPENERGYTYPE_H
#define IGL_ARAPENERGYTYPE_H
namespace igl
{
  /// Enum for choosing ARAP energy type
  enum ARAPEnergyType
  {
    /// "As-rigid-as-possible Surface Modeling" by [Sorkine and Alexa 2007],
    /// rotations defined at vertices affecting incident edges, default
    ARAP_ENERGY_TYPE_SPOKES = 0,
    /// Adapted version of "As-rigid-as-possible Surface Modeling" by [Sorkine
    /// and Alexa 2007] presented in section 4.2 of or "A simple geometric model
    /// for elastic deformation" by [Chao et al.\ 2010], rotations defined at
    /// vertices affecting incident edges and opposite edges
    ARAP_ENERGY_TYPE_SPOKES_AND_RIMS = 1,
    /// "A local-global approach to mesh parameterization" by [Liu et al.\ 2010]
    /// or "A simple geometric model for elastic deformation" by [Chao et al.\ 2010], rotations defined at elements (triangles or tets) 
    ARAP_ENERGY_TYPE_ELEMENTS = 2,
    /// Choose one automatically: spokes and rims for surfaces, elements for
    /// planar meshes and tets (not fully supported)
    ARAP_ENERGY_TYPE_DEFAULT = 3,
    /// Total number of types
    NUM_ARAP_ENERGY_TYPES = 4
  };
}
#endif
