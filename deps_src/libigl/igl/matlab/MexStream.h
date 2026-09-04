// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_MATLAB_MEX_STREAM_H
#define IGL_MATLAB_MEX_STREAM_H
#include <iostream>
namespace igl
{
  namespace matlab
  {
    
    /// Class to implement "cout" for mex files to print to the matlab terminal
    /// window.
    ///
    /// \code{cpp}
    ///  // very beginning of mexFunction():
    ///  MexStream mout;
    ///  std::streambuf *outbuf = std::cout.rdbuf(&mout); 
    ///  ...
    ///  // ALWAYS restore original buffer to avoid memory leaks in matlab
    ///  std::cout.rdbuf(outbuf);
    /// \encode
    ///
    /// http://stackoverflow.com/a/249008/148668
    class MexStream : public std::streambuf
    {
      public:
      protected:
        inline virtual std::streamsize xsputn(const char *s, std::streamsize n); 
        inline virtual int overflow(int c = EOF);
    }; 
  }
}

// Implementation 
#include <mex.h>
inline std::streamsize igl::matlab::MexStream::xsputn(
  const char *s, 
  std::streamsize n) 
{
  mexPrintf("%.*s",n,s);
  mexEvalString("drawnow;"); // to dump string.
  return n;
}

inline int igl::matlab::MexStream::overflow(int c) 
{
    if (c != EOF) {
      mexPrintf("%.1s",&c);
      mexEvalString("drawnow;"); // to dump string.
    }
    return 1;
}
#endif
