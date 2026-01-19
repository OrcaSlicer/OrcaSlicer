/*<html><pre>  -<a                             href="qh-user_r.htm"
  >-------------------------------</a><a name="TOP">-</a>

   usermem_r-cpp.cpp

   Redefine qh_exit() as 'throw std::runtime_error("QH10003 ...")'

   This file is not included in the Qhull builds.

   qhull_r calls qh_exit() when qh_errexit() is not available.  For example,
   it calls qh_exit() if you linked the wrong qhull library.

   The C++ interface avoids most of the calls to qh_exit().

   If needed, include usermem_r-cpp.o before libqhullstatic_r.a.  You may need to
   override duplicate symbol errors (e.g. /FORCE:MULTIPLE for DevStudio).  It
   may produce a warning about throwing an error from C code.
*/

extern "C" {
    void    qh_exit(int exitcode);
}

#include <stdexcept>
#include <stdlib.h>

/*-<a                             href="qh-user_r.htm#TOC"
  >-------------------------------</a><a name="qh_exit">-</a>

  qh_exit( exitcode )
    exit program

  notes:
    same as exit()
*/
void qh_exit(int exitcode) {
    exitcode= exitcode;
    throw std::runtime_error("QH10003 Qhull error.  See stderr or errfile.");
} /* exit */

/*-<a                             href="qh-user_r.htm#TOC"
  >-------------------------------</a><a name="qh_fprintf_stderr">-</a>

  qh_fprintf_stderr( msgcode, format, list of args )
    fprintf to stderr with msgcode (non-zero)

  notes:
    qh_fprintf_stderr() is called when qh->ferr is not defined, usually due to an initialization error

    It is typically followed by qh_errexit().

    Redefine this function to avoid using stderr

    Use qh_fprintf [userprintf_r.c] for normal printing
*/
void qh_fprintf_stderr(int msgcode, const char *fmt, ... ) {
    va_list args;

    va_start(args, fmt);
    if(msgcode)
      fprintf(stderr, "QH%.4d ", msgcode);
    vfprintf(stderr, fmt, args);
    va_end(args);
} /* fprintf_stderr */

/*-<a                             href="qh-user_r.htm#TOC"
>-------------------------------</a><a name="qh_free">-</a>

  qh_free(qhT *qh, mem )
    free memory

  notes:
    same as free()
    No calls to qh_errexit()
*/
void qh_free(void *mem) {
    free(mem);
} /* free */

/*-<a                             href="qh-user_r.htm#TOC"
    >-------------------------------</a><a name="qh_malloc">-</a>

    qh_malloc( mem )
      allocate memory

    notes:
      same as malloc()
*/
void *qh_malloc(size_t size) {
    return malloc(size);
} /* malloc */


