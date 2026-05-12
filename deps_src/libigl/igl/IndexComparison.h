// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_INDEXCOMPARISON_H
#define IGL_INDEXCOMPARISON_H
namespace igl{
  /// Comparison struct used by sort
  /// http://bytes.com/topic/c/answers/132045-sort-get-index
  template<class T> struct IndexLessThan
  {
    IndexLessThan(const T arr) : arr(arr) {}
    bool operator()(const size_t a, const size_t b) const
    {
      return arr[a] < arr[b];
    }
    const T arr;
  };

  /// Comparison struct used by unique
  template<class T> struct IndexEquals
  {
    IndexEquals(const T arr) : arr(arr) {}
    bool operator()(const size_t a, const size_t b) const
    {
      return arr[a] == arr[b];
    }
    const T arr;
  };

  /// Comparison struct for vectors for use with functions like std::sort
  template<class T> struct IndexVectorLessThan
  {
    IndexVectorLessThan(const T & vec) : vec ( vec) {}
    bool operator()(const size_t a, const size_t b) const
    {
      return vec(a) < vec(b);
    }
    const T & vec;
  };

  /// Comparison struct for use with functions like std::sort
  template<class T> struct IndexDimLessThan
  {
    IndexDimLessThan(const T & mat,const int & dim, const int & j) : 
      mat(mat),
      dim(dim),
      j(j)
    {}
    bool operator()(const size_t a, const size_t b) const
    {
      if(dim == 1)
      {
        return mat(a,j) < mat(b,j);
      }else
      {
        return mat(j,a) < mat(j,b);
      }
    }
    const T & mat;
    const int & dim;
    const int & j;
  };

  /// Comparison struct For use with functions like std::sort
  template<class T> struct IndexRowLessThan
  {
    IndexRowLessThan(const T & mat) : mat ( mat) {}
    bool operator()(const size_t a, const size_t b) const
    {
      const int cols = mat.cols();
      // Lexicographical order
      for(int j = 0;j<cols;j++)
      {
        if(mat(a,j) > mat(b,j))
        {
          return false;
        } else if(mat(a,j) < mat(b,j))
        {
          return true;
        }
      }
      // equality is false
      return false;
    }
    const T & mat;
  };

  /// Comparison struct for use with functions like std::sort
  template<class T> struct IndexRowEquals
  {
    IndexRowEquals(const T & mat) : mat ( mat) {}
    bool operator()(const size_t a, const size_t b) const
    {
      const int cols = mat.cols();
      // Lexicographical order
      for(int j = 0;j<cols;j++)
      {
        if(mat(a,j) !=  mat(b,j))
        {
          return false;
        }
      }
      return true;
    }
    const T & mat;
  };

}

#endif
