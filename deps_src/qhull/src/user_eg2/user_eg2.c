/*<html><pre>  -<a                             href="../libqhull/qh-user.htm"
  >-------------------------------</a><a name="TOP">-</a>

  user_eg2.c

  sample code for calling qhull() from an application.

  See user_eg.c for a simpler method using qh_new_qhull().
  The method used here and in unix.c gives you additional
  control over Qhull.

  See user_eg3/user_eg3_r.cpp for a C++ example

  call with:

     user_eg2 "triangulated cube/diamond options" "delaunay options" "halfspace options"

  for example:

     user_eg2                             # return summaries

     user_eg2 "n" "o" "Fp"                # return normals, OFF, points

     user_eg2 "QR0 p" "QR0 v p" "QR0 Fp"  # rotate input and return points
                                         # 'v' returns Voronoi
                                         # transform is rotated for halfspaces

   main() makes three runs of qhull.

     1) compute the convex hull of a cube, and incrementally add a diamond

     2a) compute the Delaunay triangulation of random points, and add points.

     2b) find the Delaunay triangle closest to a point.

     3) compute the halfspace intersection of a diamond, and add a cube

 notes:

   summaries are sent to stderr if other output formats are used

   derived from unix.c and compiled by 'make bin/user_eg2'

   see libqhull.h for data structures, macros, and user-callable functions.

   If you want to control all output to stdio and input to stdin,
   set the #if below to "1" and delete all lines that contain "io.c".
   This prevents the loading of io.o.  Qhull will
   still write to 'qh ferr' (stderr) for error reporting and tracing.

   Defining #if 1, also prevents user.o from being loaded.
*/

#include "libqhull/qhull_a.h"

/*-------------------------------------------------
-internal function prototypes
*/
void print_summary(void);
void makecube(coordT *points, int numpoints, int dim);
void adddiamond(coordT *points, int numpoints, int numnew, int dim);
void makeDelaunay(coordT *points, int numpoints, int dim);
void addDelaunay(coordT *points, int numpoints, int numnew, int dim);
void findDelaunay(int dim);
void makehalf(coordT *points, int numpoints, int dim);
void addhalf(coordT *points, int numpoints, int numnew, int dim, coordT *feasible);

/*-------------------------------------------------
-print_summary()
*/
void print_summary(void) {
  facetT *facet;
  int k;

  printf("\n%d vertices and %d facets with normals:\n",
                 qh num_vertices, qh num_facets);
  FORALLfacets {
    for (k=0; k < qh hull_dim; k++)
      printf("%6.2g ", facet->normal[k]);
    printf("\n");
  }
}

/*--------------------------------------------------
-makecube- set points to vertices of cube
  points is numpoints X dim
*/
void makecube(coordT *points, int numpoints, int dim) {
  int j,k;
  coordT *point;

  for (j=0; j<numpoints; j++) {
    point= points + j*dim;
    for (k=dim; k--; ) {
      if (j & ( 1 << k))
        point[k]= 1.0;
      else
        point[k]= -1.0;
    }
  }
} /*.makecube.*/

/*--------------------------------------------------
-adddiamond- add diamond to convex hull
  points is numpoints+numnew X dim.

notes:
  qh_addpoint() does not make a copy of the point coordinates.

  For inside points and some outside points, qh_findbestfacet performs
  an exhaustive search for a visible facet.  Algorithms that retain
  previously constructed hulls should be faster for on-line construction
  of the convex hull.
*/
void adddiamond(coordT *points, int numpoints, int numnew, int dim) {
  int j,k;
  coordT *point;
  facetT *facet;
  boolT isoutside;
  realT bestdist;

  for (j= 0; j < numnew ; j++) {
    point= points + (numpoints+j)*dim;
    if (points == qh first_point)  /* in case of 'QRn' */
      qh num_points= numpoints+j+1;
    /* qh.num_points sets the size of the points array.  You may
       allocate the points elsewhere.  If so, qh_addpoint records
       the point's address in qh other_points
    */
    for (k=dim; k--; ) {
      if (j/2 == k)
        point[k]= (j & 1) ? 2.0 : -2.0;
      else
        point[k]= 0.0;
    }
    facet= qh_findbestfacet(point, !qh_ALL, &bestdist, &isoutside);
    if (isoutside) {
      if (!qh_addpoint(point, facet, False))
        break;  /* user requested an early exit with 'TVn' or 'TCn' */
    }
    printf("%d vertices and %d facets\n",
                 qh num_vertices, qh num_facets);
    /* qh_produce_output(); */
  }
  if (qh DOcheckmax)
    qh_check_maxout();
  else if (qh KEEPnearinside)
    qh_nearcoplanar();
} /*.adddiamond.*/

/*--------------------------------------------------
-makeDelaunay- set points for dim-1 Delaunay triangulation of random points
  points is numpoints X dim.  Each point is projected to a paraboloid.
*/
void makeDelaunay(coordT *points, int numpoints, int dim) {
  int j,k, seed;
  coordT *point, realr;

  seed= (int)time(NULL); /* time_t to int */
  printf("seed: %d\n", seed);
  qh_RANDOMseed_( seed);
  for (j=0; j<numpoints; j++) {
    point= points + j*dim;
    for (k= 0; k < dim-1; k++) {
      realr= qh_RANDOMint;
      point[k]= 2.0 * realr/(qh_RANDOMmax+1) - 1.0;
    }
  }
  qh_setdelaunay(dim, numpoints, points);
} /*.makeDelaunay.*/

/*--------------------------------------------------
-addDelaunay- add points to dim-1 Delaunay triangulation
  points is numpoints+numnew X dim.  Each point is projected to a paraboloid.
notes:
  qh_addpoint() does not make a copy of the point coordinates.

  Since qh_addpoint() is not given a visible facet, it performs a directed
  search of all facets.  Algorithms that retain previously
  constructed hulls may be faster.
*/
void addDelaunay(coordT *points, int numpoints, int numnew, int dim) {
  int j,k;
  coordT *point, realr;
  facetT *facet;
  realT bestdist;
  boolT isoutside;

  for (j= 0; j < numnew ; j++) {
    point= points + (numpoints+j)*dim;
    if (points == qh first_point)  /* in case of 'QRn' */
      qh num_points= numpoints+j+1;
    /* qh.num_points sets the size of the points array.  You may
       allocate the point elsewhere.  If so, qh_addpoint records
       the point's address in qh other_points
    */
    for (k= 0; k < dim-1; k++) {
      realr= qh_RANDOMint;
      point[k]= 2.0 * realr/(qh_RANDOMmax+1) - 1.0;
    }
    qh_setdelaunay(dim, 1, point);
    facet= qh_findbestfacet(point, !qh_ALL, &bestdist, &isoutside);
    if (isoutside) {
      if (!qh_addpoint(point, facet, False))
        break;  /* user requested an early exit with 'TVn' or 'TCn' */
    }
    qh_printpoint(stdout, "added point", point);
    printf("%d points, %d extra points, %d vertices, and %d facets in total\n",
                  qh num_points, qh_setsize(qh other_points),
                  qh num_vertices, qh num_facets);

    /* qh_produce_output(); */
  }
  if (qh DOcheckmax)
    qh_check_maxout();
  else if (qh KEEPnearinside)
    qh_nearcoplanar();
} /*.addDelaunay.*/

/*--------------------------------------------------
-findDelaunay- find Delaunay triangle for [0.5,0.5,...]
  assumes dim < 100
notes:
  calls qh_setdelaunay() to project the point to a parabaloid
warning:
  This is not implemented for tricoplanar facets ('Qt'),
  See <a href="../html/qh-code.htm#findfacet">locate a facet with qh_findbestfacet()</a>
*/
void findDelaunay(int dim) {
  int k;
  coordT point[ 100];
  boolT isoutside;
  realT bestdist;
  facetT *facet;
  vertexT *vertex, **vertexp;

  for (k= 0; k < dim-1; k++)
    point[k]= 0.5;
  qh_setdelaunay(dim, 1, point);
  facet= qh_findbestfacet(point, qh_ALL, &bestdist, &isoutside);
  if (facet->tricoplanar) {
    fprintf(stderr, "findDelaunay: not implemented for triangulated, non-simplicial Delaunay regions (tricoplanar facet, f%d).\n",
       facet->id);
    qh_errexit(qh_ERRqhull, facet, NULL);
  }
  FOREACHvertex_(facet->vertices) {
    for (k=0; k < dim-1; k++)
      printf("%5.2f ", vertex->point[k]);
    printf("\n");
  }
} /*.findDelaunay.*/

/*--------------------------------------------------
-makehalf- set points to halfspaces for a (dim)-d diamond
  points is numpoints X dim+1

  each halfspace consists of dim coefficients followed by an offset
*/
void makehalf(coordT *points, int numpoints, int dim) {
  int j,k;
  coordT *point;

  for (j=0; j<numpoints; j++) {
    point= points + j*(dim+1);
    point[dim]= -1.0; /* offset */
    for (k=dim; k--; ) {
      if (j & ( 1 << k))
        point[k]= 1.0;
      else
        point[k]= -1.0;
    }
  }
} /*.makehalf.*/

/*--------------------------------------------------
-addhalf- add halfspaces for a (dim)-d cube to the intersection
  points is numpoints+numnew X dim+1
notes:
  assumes dim < 100.

  For makehalf(), points is the initial set of halfspaces with offsets.
  It is transformed by qh_sethalfspace_all into a
  (dim)-d set of newpoints.  Qhull computed the convex hull of newpoints -
  this is equivalent to the halfspace intersection of the
  orginal halfspaces.

  For addhalf(), the remainder of points stores the transforms of
  the added halfspaces.  Qhull computes the convex hull of newpoints
  and the added points.  qh_addpoint() does not make a copy of these points.

  Since halfspace intersection is equivalent to a convex hull,
  qh_findbestfacet may perform an exhaustive search
  for a visible facet.  Algorithms that retain previously constructed
  intersections should be faster for on-line construction.
*/
void addhalf(coordT *points, int numpoints, int numnew, int dim, coordT *feasible) {
  int j,k;
  coordT *point, normal[100], offset, *next;
  facetT *facet;
  boolT isoutside;
  realT bestdist;

  for (j= 0; j < numnew ; j++) {
    offset= -1.0;
    for (k=dim; k--; ) {
      if (j/2 == k) {
        normal[k]= sqrt((coordT)dim);   /* to normalize as in makehalf */
        if (j & 1)
          normal[k]= -normal[k];
      }else
        normal[k]= 0.0;
    }
    point= points + (numpoints+j)* (dim+1);  /* does not use point[dim] */
    qh_sethalfspace(dim, point, &next, normal, &offset, feasible);
    facet= qh_findbestfacet(point, !qh_ALL, &bestdist, &isoutside);
    if (isoutside) {
      if (!qh_addpoint(point, facet, False))
        break;  /* user requested an early exit with 'TVn' or 'TCn' */
    }
    qh_printpoint(stdout, "added offset -1 and normal", normal);
    printf("%d points, %d extra points, %d vertices, and %d facets in total\n",
                  qh num_points, qh_setsize(qh other_points),
                  qh num_vertices, qh num_facets);
    /* qh_produce_output(); */
  }
  if (qh DOcheckmax)
    qh_check_maxout();
  else if (qh KEEPnearinside)
    qh_nearcoplanar();
} /*.addhalf.*/

#define DIM 3     /* dimension of points, must be < 31 for SIZEcube */
#define SIZEcube (1<<DIM)
#define SIZEdiamond (2*DIM)
#define TOTpoints (SIZEcube + SIZEdiamond)

/*--------------------------------------------------
-main- derived from unix.c

  see program header

  this contains three runs of Qhull for convex hull, Delaunay
  triangulation or Voronoi vertices, and halfspace intersection

*/
int main(int argc, char *argv[]) {
  boolT ismalloc;
  int curlong, totlong, exitcode;
  char options [2000];

  QHULL_LIB_CHECK

  printf("This is the output from user_eg2.c\n\n\
It shows how qhull() may be called from an application in the same way as\n\
qconvex.  It is not part of qhull itself.  If it appears accidently,\n\
please remove user_eg2.c from your project.\n\n");

#if qh_QHpointer  /* see user.h */
  if (qh_qh){
      printf("QH6237: Qhull link error.  The global variable qh_qh was not initialized\n\
              to NULL by global.c.  Please compile user_eg2.c with -Dqh_QHpointer_dllimport\n\
              as well as -Dqh_QHpointer, or use libqhullstatic, or use a different tool chain.\n\n");
      return -1;
  }
#endif


  ismalloc= False;      /* True if qh_freeqhull should 'free(array)' */
  /*
    Run 1: convex hull
  */
  qh_init_A(stdin, stdout, stderr, 0, NULL);
  exitcode= setjmp(qh errexit);
  if (!exitcode) {
    coordT array[TOTpoints][DIM];

    strcat(qh rbox_command, "user_eg cube");
    sprintf(options, "qhull s Tcv Q11 %s ", argc >= 2 ? argv[1] : "");
    qh_initflags(options);
    printf( "\ncompute triangulated convex hull of cube after rotating input\n");
    makecube(array[0], SIZEcube, DIM);
    qh_init_B(array[0], SIZEcube, DIM, ismalloc);
    qh_qhull();
    qh_check_output();
    qh_triangulate();  /* requires option 'Q11' if want to add points */
    print_summary();
    if (qh VERIFYoutput && !qh STOPpoint && !qh STOPcone)
      qh_check_points();
    printf( "\nadd points in a diamond\n");
    adddiamond(array[0], SIZEcube, SIZEdiamond, DIM);
    qh_check_output();
    print_summary();
    qh_produce_output();  /* delete this line to help avoid io.c */
    if (qh VERIFYoutput && !qh STOPpoint && !qh STOPcone)
      qh_check_points();
  }
  qh NOerrexit= True;
  qh_freeqhull(!qh_ALL);
  qh_memfreeshort(&curlong, &totlong);
  if (curlong || totlong)
    fprintf(stderr, "qhull warning (user_eg, run 1): did not free %d bytes of long memory (%d pieces)\n",
       totlong, curlong);

  /*
    Run 2: Delaunay triangulation
  */
  qh_init_A(stdin, stdout, stderr, 0, NULL);
  exitcode= setjmp(qh errexit);
  if (!exitcode) {
    coordT array[TOTpoints][DIM];

    strcat(qh rbox_command, "user_eg Delaunay");
    sprintf(options, "qhull s d Tcv %s", argc >= 3 ? argv[2] : "");
    qh_initflags(options);
    printf( "\ncompute %d-d Delaunay triangulation\n", DIM-1);
    makeDelaunay(array[0], SIZEcube, DIM);
    /* Instead of makeDelaunay with qh_setdelaunay, you may
       produce a 2-d array of points, set DIM to 2, and set
       qh PROJECTdelaunay to True.  qh_init_B will call
       qh_projectinput to project the points to the paraboloid
       and add a point "at-infinity".
    */
    qh_init_B(array[0], SIZEcube, DIM, ismalloc);
    qh_qhull();
    /* If you want Voronoi ('v') without qh_produce_output(), call
       qh_setvoronoi_all() after qh_qhull() */
    qh_check_output();
    print_summary();
    qh_produce_output();  /* delete this line to help avoid io.c */
    if (qh VERIFYoutput && !qh STOPpoint && !qh STOPcone)
      qh_check_points();
    printf( "\nadd points to triangulation\n");
    addDelaunay(array[0], SIZEcube, SIZEdiamond, DIM);
    qh_check_output();
    qh_produce_output();  /* delete this line to help avoid io.c */
    if (qh VERIFYoutput && !qh STOPpoint && !qh STOPcone)
      qh_check_points();
    printf( "\nfind Delaunay triangle closest to [0.5, 0.5, ...]\n");
    findDelaunay(DIM);
  }
  qh NOerrexit= True;
  qh_freeqhull(!qh_ALL);
  qh_memfreeshort(&curlong, &totlong);
  if (curlong || totlong)
    fprintf(stderr, "qhull warning (user_eg, run 2): did not free %d bytes of long memory (%d pieces)\n",
       totlong, curlong);

  /*
    Run 3: halfspace intersection
  */
  qh_init_A(stdin, stdout, stderr, 0, NULL);
  exitcode= setjmp(qh errexit);
  if (!exitcode) {
    coordT array[TOTpoints][DIM+1];  /* +1 for halfspace offset */
    pointT *points;

    strcat(qh rbox_command, "user_eg halfspaces");
    sprintf(options, "qhull H0 s Tcv %s", argc >= 4 ? argv[3] : "");
    qh_initflags(options);
    printf( "\ncompute halfspace intersection about the origin for a diamond\n");
    makehalf(array[0], SIZEcube, DIM);
    qh_setfeasible(DIM); /* from io.c, sets qh feasible_point from 'Hn,n' */
    /* you may malloc and set qh feasible_point directly.  It is only used for
       option 'Fp' */
    points= qh_sethalfspace_all( DIM+1, SIZEcube, array[0], qh feasible_point);
    qh_init_B(points, SIZEcube, DIM, True); /* qh_freeqhull frees points */
    qh_qhull();
    qh_check_output();
    qh_produce_output();  /* delete this line to help avoid io.c */
    if (qh VERIFYoutput && !qh STOPpoint && !qh STOPcone)
      qh_check_points();
    printf( "\nadd halfspaces for cube to intersection\n");
    addhalf(array[0], SIZEcube, SIZEdiamond, DIM, qh feasible_point);
    qh_check_output();
    qh_produce_output();  /* delete this line to help avoid io.c */
    if (qh VERIFYoutput && !qh STOPpoint && !qh STOPcone)
      qh_check_points();
  }
  qh NOerrexit= True;
  qh NOerrexit= True;
  qh_freeqhull(!qh_ALL);
  qh_memfreeshort(&curlong, &totlong);
  if (curlong || totlong)
    fprintf(stderr, "qhull warning (user_eg, run 3): did not free %d bytes of long memory (%d pieces)\n",
       totlong, curlong);
  return exitcode;
} /* main */

#if 1    /* use 1 to prevent loading of io.o and user.o */
/*-------------------------------------------
-errexit- return exitcode to system after an error
  assumes exitcode non-zero
  prints useful information
  see qh_errexit2() in libqhull.c for 2 facets
*/
void qh_errexit(int exitcode, facetT *facet, ridgeT *ridge) {
  QHULL_UNUSED(facet);
  QHULL_UNUSED(ridge);

  if (qh ERREXITcalled) {
    fprintf(qh ferr, "qhull error while processing previous error.  Exit program\n");
    exit(1);
  }
  qh ERREXITcalled= True;
  if (!qh QHULLfinished)
    qh hulltime= (unsigned)clock() - qh hulltime;
  fprintf(qh ferr, "\nWhile executing: %s | %s\n", qh rbox_command, qh qhull_command);
  fprintf(qh ferr, "Options selected:\n%s\n", qh qhull_options);
  if (qh furthest_id >= 0) {
    fprintf(qh ferr, "\nLast point added to hull was p%d", qh furthest_id);
    if (zzval_(Ztotmerge))
      fprintf(qh ferr, "  Last merge was #%d.", zzval_(Ztotmerge));
    if (qh QHULLfinished)
      fprintf(qh ferr, "\nQhull has finished constructing the hull.");
    else if (qh POSTmerging)
      fprintf(qh ferr, "\nQhull has started post-merging");
    fprintf(qh ferr, "\n\n");
  }
  if (qh NOerrexit) {
    fprintf(qh ferr, "qhull error while ending program.  Exit program\n");
    exit(1);
  }
  if (!exitcode)
    exitcode= qh_ERRqhull;
  qh NOerrexit= True;
  longjmp(qh errexit, exitcode);
} /* errexit */


/*-------------------------------------------
-errprint- prints out the information of the erroneous object
    any parameter may be NULL, also prints neighbors and geomview output
*/
void qh_errprint(const char *string, facetT *atfacet, facetT *otherfacet, ridgeT *atridge, vertexT *atvertex) {

  fprintf(qh ferr, "%s facets f%d f%d ridge r%d vertex v%d\n",
           string, getid_(atfacet), getid_(otherfacet), getid_(atridge),
           getid_(atvertex));
} /* errprint */


void qh_printfacetlist(facetT *facetlist, setT *facets, boolT printall) {
  facetT *facet, **facetp;

  /* remove these calls to help avoid io.c */
  qh_printbegin(qh ferr, qh_PRINTfacets, facetlist, facets, printall);/*io.c*/
  FORALLfacet_(facetlist)                                              /*io.c*/
    qh_printafacet(qh ferr, qh_PRINTfacets, facet, printall);          /*io.c*/
  FOREACHfacet_(facets)                                                /*io.c*/
    qh_printafacet(qh ferr, qh_PRINTfacets, facet, printall);          /*io.c*/
  qh_printend(qh ferr, qh_PRINTfacets, facetlist, facets, printall);  /*io.c*/

  FORALLfacet_(facetlist)
    fprintf( qh ferr, "facet f%d\n", facet->id);
} /* printfacetlist */

/* qh_printhelp_degenerate( fp )
    prints descriptive message for precision error

  notes:
    no message if qh_QUICKhelp
*/
void qh_printhelp_degenerate(FILE *fp) {

  if (qh MERGEexact || qh PREmerge || qh JOGGLEmax < REALmax/2)
    qh_fprintf(fp, 9368, "\n\
A Qhull error has occurred.  Qhull should have corrected the above\n\
precision error.  Please send the input and all of the output to\n\
qhull_bug@qhull.org\n");
  else if (!qh_QUICKhelp) {
    qh_fprintf(fp, 9369, "\n\
Precision problems were detected during construction of the convex hull.\n\
This occurs because convex hull algorithms assume that calculations are\n\
exact, but floating-point arithmetic has roundoff errors.\n\
\n\
To correct for precision problems, do not use 'Q0'.  By default, Qhull\n\
selects 'C-0' or 'Qx' and merges non-convex facets.  With option 'QJ',\n\
Qhull joggles the input to prevent precision problems.  See \"Imprecision\n\
in Qhull\" (qh-impre.htm).\n\
\n\
If you use 'Q0', the output may include\n\
coplanar ridges, concave ridges, and flipped facets.  In 4-d and higher,\n\
Qhull may produce a ridge with four neighbors or two facets with the same \n\
vertices.  Qhull reports these events when they occur.  It stops when a\n\
concave ridge, flipped facet, or duplicate facet occurs.\n");
#if REALfloat
    qh_fprintf(fp, 9370, "\
\n\
Qhull is currently using single precision arithmetic.  The following\n\
will probably remove the precision problems:\n\
  - recompile qhull for realT precision(#define REALfloat 0 in user.h).\n");
#endif
    if (qh DELAUNAY && !qh SCALElast && qh MAXabs_coord > 1e4)
      qh_fprintf(fp, 9371, "\
\n\
When computing the Delaunay triangulation of coordinates > 1.0,\n\
  - use 'Qbb' to scale the last coordinate to [0,m] (max previous coordinate)\n");
    if (qh DELAUNAY && !qh ATinfinity)
      qh_fprintf(fp, 9372, "\
When computing the Delaunay triangulation:\n\
  - use 'Qz' to add a point at-infinity.  This reduces precision problems.\n");

    qh_fprintf(fp, 9373, "\
\n\
If you need triangular output:\n\
  - use option 'Qt' to triangulate the output\n\
  - use option 'QJ' to joggle the input points and remove precision errors\n\
  - use option 'Ft'.  It triangulates non-simplicial facets with added points.\n\
\n\
If you must use 'Q0',\n\
try one or more of the following options.  They can not guarantee an output.\n\
  - use 'QbB' to scale the input to a cube.\n\
  - use 'Po' to produce output and prevent partitioning for flipped facets\n\
  - use 'V0' to set min. distance to visible facet as 0 instead of roundoff\n\
  - use 'En' to specify a maximum roundoff error less than %2.2g.\n\
  - options 'Qf', 'Qbb', and 'QR0' may also help\n",
               qh DISTround);
    qh_fprintf(fp, 9374, "\
\n\
To guarantee simplicial output:\n\
  - use option 'Qt' to triangulate the output\n\
  - use option 'QJ' to joggle the input points and remove precision errors\n\
  - use option 'Ft' to triangulate the output by adding points\n\
  - use exact arithmetic (see \"Imprecision in Qhull\", qh-impre.htm)\n\
");
  }
} /* printhelp_degenerate */


/* qh_printhelp_narrowhull( minangle )
     Warn about a narrow hull

  notes:
    Alternatively, reduce qh_WARNnarrow in user.h

*/
void qh_printhelp_narrowhull(FILE *fp, realT minangle) {

    qh_fprintf(fp, 9375, "qhull precision warning: \n\
The initial hull is narrow (cosine of min. angle is %.16f).\n\
A coplanar point may lead to a wide facet.  Options 'QbB' (scale to unit box)\n\
or 'Qbb' (scale last coordinate) may remove this warning.  Use 'Pp' to skip\n\
this warning.  See 'Limitations' in qh-impre.htm.\n",
          -minangle);   /* convert from angle between normals to angle between facets */
} /* printhelp_narrowhull */

/* qh_printhelp_singular
      prints descriptive message for singular input
*/
void qh_printhelp_singular(FILE *fp) {
  facetT *facet;
  vertexT *vertex, **vertexp;
  realT min, max, *coord, dist;
  int i,k;

  qh_fprintf(fp, 9376, "\n\
The input to qhull appears to be less than %d dimensional, or a\n\
computation has overflowed.\n\n\
Qhull could not construct a clearly convex simplex from points:\n",
           qh hull_dim);
  qh_printvertexlist(fp, "", qh facet_list, NULL, qh_ALL);
  if (!qh_QUICKhelp)
    qh_fprintf(fp, 9377, "\n\
The center point is coplanar with a facet, or a vertex is coplanar\n\
with a neighboring facet.  The maximum round off error for\n\
computing distances is %2.2g.  The center point, facets and distances\n\
to the center point are as follows:\n\n", qh DISTround);
  qh_printpointid(fp, "center point", qh hull_dim, qh interior_point, -1);
  qh_fprintf(fp, 9378, "\n");
  FORALLfacets {
    qh_fprintf(fp, 9379, "facet");
    FOREACHvertex_(facet->vertices)
      qh_fprintf(fp, 9380, " p%d", qh_pointid(vertex->point));
    zinc_(Zdistio);
    qh_distplane(qh interior_point, facet, &dist);
    qh_fprintf(fp, 9381, " distance= %4.2g\n", dist);
  }
  if (!qh_QUICKhelp) {
    if (qh HALFspace)
      qh_fprintf(fp, 9382, "\n\
These points are the dual of the given halfspaces.  They indicate that\n\
the intersection is degenerate.\n");
    qh_fprintf(fp, 9383,"\n\
These points either have a maximum or minimum x-coordinate, or\n\
they maximize the determinant for k coordinates.  Trial points\n\
are first selected from points that maximize a coordinate.\n");
    if (qh hull_dim >= qh_INITIALmax)
      qh_fprintf(fp, 9384, "\n\
Because of the high dimension, the min x-coordinate and max-coordinate\n\
points are used if the determinant is non-zero.  Option 'Qs' will\n\
do a better, though much slower, job.  Instead of 'Qs', you can change\n\
the points by randomly rotating the input with 'QR0'.\n");
  }
  qh_fprintf(fp, 9385, "\nThe min and max coordinates for each dimension are:\n");
  for (k=0; k < qh hull_dim; k++) {
    min= REALmax;
    max= -REALmin;
    for (i=qh num_points, coord= qh first_point+k; i--; coord += qh hull_dim) {
      maximize_(max, *coord);
      minimize_(min, *coord);
    }
    qh_fprintf(fp, 9386, "  %d:  %8.4g  %8.4g  difference= %4.4g\n", k, min, max, max-min);
  }
  if (!qh_QUICKhelp) {
    qh_fprintf(fp, 9387, "\n\
If the input should be full dimensional, you have several options that\n\
may determine an initial simplex:\n\
  - use 'QJ'  to joggle the input and make it full dimensional\n\
  - use 'QbB' to scale the points to the unit cube\n\
  - use 'QR0' to randomly rotate the input for different maximum points\n\
  - use 'Qs'  to search all points for the initial simplex\n\
  - use 'En'  to specify a maximum roundoff error less than %2.2g.\n\
  - trace execution with 'T3' to see the determinant for each point.\n",
                     qh DISTround);
#if REALfloat
    qh_fprintf(fp, 9388, "\
  - recompile qhull for realT precision(#define REALfloat 0 in libqhull.h).\n");
#endif
    qh_fprintf(fp, 9389, "\n\
If the input is lower dimensional:\n\
  - use 'QJ' to joggle the input and make it full dimensional\n\
  - use 'Qbk:0Bk:0' to delete coordinate k from the input.  You should\n\
    pick the coordinate with the least range.  The hull will have the\n\
    correct topology.\n\
  - determine the flat containing the points, rotate the points\n\
    into a coordinate plane, and delete the other coordinates.\n\
  - add one or more points to make the input full dimensional.\n\
");
    if (qh DELAUNAY && !qh ATinfinity)
      qh_fprintf(fp, 9390, "\n\n\
This is a Delaunay triangulation and the input is co-circular or co-spherical:\n\
  - use 'Qz' to add a point \"at infinity\" (i.e., above the paraboloid)\n\
  - or use 'QJ' to joggle the input and avoid co-circular data\n");
  }
} /* printhelp_singular */


/*-----------------------------------------
-user_memsizes- allocate up to 10 additional, quick allocation sizes
*/
void qh_user_memsizes(void) {

  /* qh_memsize(size); */
} /* user_memsizes */

#endif
