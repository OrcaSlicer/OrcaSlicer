// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_REDRUM_H
#define IGL_REDRUM_H

// Q: These should probably be inside the igl namespace. What's the correct
// way to do that?
// A: I guess the right way is to not use a macro but a proper function with
// streams as input and output.

// ANSI color codes for formatting iostream style output

#ifdef IGL_REDRUM_NOOP

// Bold Red, etc.
#define NORUM(X)     X
#define REDRUM(X)     X
#define GREENRUM(X)   X
#define YELLOWRUM(X)  X
#define BLUERUM(X)    X
#define MAGENTARUM(X) X
#define CYANRUM(X)    X
// Regular Red, etc.
#define REDGIN(X)     X
#define GREENGIN(X)   X
#define YELLOWGIN(X)  X
#define BLUEGIN(X)    X
#define MAGENTAGIN(X) X
#define CYANGIN(X)    X

#else

/// Bold red colored text
/// @param[in] X  text to color
/// @returns colored text as "stream"
/// #### Example:
///
/// \code{cpp}
/// std::cout<<REDRUM("File "<<filename<<" not found.")<<std::endl;
/// \endcode
#define REDRUM(X)      "\e[1m\e[31m"<<X<<"\e[m"
// Bold Red, etc.
#define NORUM(X)       ""<<X<<""
#define GREENRUM(X)    "\e[1m\e[32m"<<X<<"\e[m"
#define YELLOWRUM(X)   "\e[1m\e[33m"<<X<<"\e[m"
#define BLUERUM(X)     "\e[1m\e[34m"<<X<<"\e[m"
#define MAGENTARUM(X)  "\e[1m\e[35m"<<X<<"\e[m"
#define CYANRUM(X)     "\e[1m\e[36m"<<X<<"\e[m"
// Regular Red, etc.
#define REDGIN(X)      "\e[31m"<<X<<"\e[m"
#define GREENGIN(X)    "\e[32m"<<X<<"\e[m"
#define YELLOWGIN(X)   "\e[33m"<<X<<"\e[m"
#define BLUEGIN(X)     "\e[34m"<<X<<"\e[m"
#define MAGENTAGIN(X)  "\e[35m"<<X<<"\e[m"
#define CYANGIN(X)     "\e[36m"<<X<<"\e[m"
#endif

#endif 
