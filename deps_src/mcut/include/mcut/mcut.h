/**
 * Copyright (c) 2021-2022 Floyd M. Chitalu.
 * All rights reserved.
 *
 * NOTE: This file is licensed under GPL-3.0-or-later (default).
 * A commercial license can be purchased from Floyd M. Chitalu.
 *
 * License details:
 *
 * (A)  GNU General Public License ("GPL"); a copy of which you should have
 *      recieved with this file.
 * 	    - see also: <http://www.gnu.org/licenses/>
 * (B)  Commercial license.
 *      - email: floyd.m.chitalu@gmail.com
 *
 * The commercial license options is for users that wish to use MCUT in
 * their products for comercial purposes but do not wish to release their
 * software products under the GPL license.
 *
 * Author(s)     : Floyd M. Chitalu
 */

/**
 * @file mcut.h
 * @author Floyd M. Chitalu
 * @date 1 Jan 2021
 *
 * @brief File containing the MCUT applications programming interface (API).
 *
 * NOTE: This header file defines all the functionality and accessible features of MCUT.
 * The interface is a standard C API.
 *
 */

#ifndef MCUT_API_H_
#define MCUT_API_H_

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

#include "platform.h"

/** @file */

/** Macro to encode MCUT version */
#define MC_MAKE_VERSION(major, minor, patch) \
    (((major) << 22) | ((minor) << 12) | (patch))

/** MCUT 1.0 version number */
#define MC_API_VERSION_1_0 MC_MAKE_VERSION(1, 0, 0) // Patch version should always be set to 0

/** MCUT 1.1 version number */
#define MC_API_VERSION_1_1 MC_MAKE_VERSION(1, 1, 0) // Patch version should always be set to 0

/** MCUT 1.2 version number */
#define MC_API_VERSION_1_2 MC_MAKE_VERSION(1, 2, 0) // Patch version should always be set to 0

/** Macro to decode MCUT version (MAJOR) from MC_HEADER_VERSION_COMPLETE */
#define MC_VERSION_MAJOR(version) ((uint32_t)(version) >> 22)

/** Macro to decode MCUT version (MINOR) from MC_HEADER_VERSION_COMPLETE */
#define MC_VERSION_MINOR(version) (((uint32_t)(version) >> 12) & 0x3ff)

/** Macro to decode MCUT version (PATCH) from MC_HEADER_VERSION_COMPLETE */
#define MC_VERSION_PATCH(version) ((uint32_t)(version)&0xfff)

/** Version of this file */
#define MC_HEADER_VERSION 100

/** Complete version of this file */
#define MC_HEADER_VERSION_COMPLETE MC_MAKE_VERSION(1, 2, MC_HEADER_VERSION)

/** Constant value assigned to null variables and parameters */
#define MC_NULL_HANDLE 0

/** Helper-macro to define opaque handles */
#define MC_DEFINE_HANDLE(object) typedef struct object##_T* object;

/** A garbage value that is assigned to undefined index values */
#define MC_UNDEFINED_VALUE UINT32_MAX

/**
 * @brief Connected component handle.
 *
 * Opaque type referencing a connected component which the client/user must use to access mesh data after a dispatch call.
 */
typedef struct McConnectedComponent_T* McConnectedComponent;

/**
 * @brief Context handle.
 *
 * Opaque type referencing a working state (e.g. independent thread) which the client/user must use to initialise, dispatch, and access data.
 */
typedef struct McContext_T* McContext;

/**
 * @brief Event handle.
 *
 * Opaque type that can be used to identify async tasks.
 */
typedef struct McEvent_T* McEvent;

typedef void McVoid;

/**
 * @brief 8 bit signed char.
 *
 * Integral type representing a 8-bit signed char.
 */
typedef int8_t McChar;

/**
 * @brief 32 bit signed integer.
 *
 * Integral type representing a 32-bit signed integer.
 */
typedef int32_t McInt32;

/**
 * @brief 32 bit unsigned integer.
 *
 * Integral type representing a 32-bit usigned integer.
 */
typedef uint32_t McUint32;

/**
 * @brief Bitfield type.
 *
 * Integral type representing a 32-bit bitfield for storing parameter values.
 */
typedef uint32_t McFlags;

/**
 * @brief Byte size type.
 *
 *  Unsigned integer type of the result of the sizeof operator as well as the sizeof... operator and the alignof operator.
 */
typedef size_t McSize;

/**
 * @brief 32-bit unsigned type.
 *
 * Integral type representing an index value.
 */
typedef uint32_t McIndex;

/**
 * @brief 32-bit floating point type.
 *
 * Floating point type type representing a 32-bit real number value.
 */
typedef float McFloat;

/**
 * @brief 64-bit floating point type.
 *
 * Floating point type type representing a 64-bit real number value.
 */
typedef double McDouble;

/**
 * @brief 32-bit type.
 *
 * Integral type representing a boolean value (MC_TRUE or MC_FALSE).
 */
typedef uint32_t McBool;

/**
 * @brief Boolean constant for "true".
 *
 * Integral constant representing a boolean value evaluating to true.
 */
#define MC_TRUE (1)

/**
 * @brief Boolean constant for "false".
 *
 * Integral constant representing a boolean value evaluating to false.
 */
#define MC_FALSE (0)

/**
 * \enum McResult
 * @brief API return codes
 *
 * This enum structure defines the possible return values of API functions (integer). The values identify whether a function executed successfully or returned with an error.
 */
typedef enum McResult {
    MC_NO_ERROR = 0, /**< The function was successfully executed. */
    MC_INVALID_OPERATION = -(1 << 1), /**< An internal operation could not be executed successively. */
    MC_INVALID_VALUE = -(1 << 2), /**< An invalid value has been passed to the API. */
    MC_OUT_OF_MEMORY = -(1 << 3), /**< Memory allocation operation cannot allocate memory. */
    MC_RESULT_MAX_ENUM = 0xFFFFFFFF /**< Wildcard (match all) . */
} McResult;

/**
 * \enum McConnectedComponentType
 * @brief The possible types of connected components.
 *
 * This enum structure defines the possible types of connected components which can be queried from the API after a dispatch call.
 */
typedef enum McConnectedComponentType {
    MC_CONNECTED_COMPONENT_TYPE_FRAGMENT = (1 << 0), /**< A connected component that originates from the source-mesh. */
    MC_CONNECTED_COMPONENT_TYPE_PATCH = (1 << 2), /**< A connected component that is originates from the cut-mesh. */
    MC_CONNECTED_COMPONENT_TYPE_SEAM = (1 << 3), /**< A connected component representing an input mesh (source-mesh or cut-mesh), but with additional vertices and edges that are introduced as as a result of the cut (i.e. the intersection contour/curve). */
    MC_CONNECTED_COMPONENT_TYPE_INPUT = (1 << 4), /**< A connected component that is copy of an input mesh (source-mesh or cut-mesh). Such a connected component may contain new faces and vertices, which will happen if MCUT internally performs polygon partitioning. Polygon partitioning occurs when an input mesh intersects the other without severing at least one edge. An example is splitting a tetrahedron (source-mesh) in two parts using one large triangle (cut-mesh): in this case, the large triangle would be partitioned into two faces to ensure that at least one of this cut-mesh are severed by the tetrahedron. This is what allows MCUT to reconnect topology after the cut. */
    MC_CONNECTED_COMPONENT_TYPE_ALL = 0xFFFFFFFF /**< Wildcard (match all) . */
} McConnectedComponentType;

/**
 * \enum McFragmentLocation
 * @brief The possible geometrical locations of a fragment (connected component), which are defined with-respect-to the cut-mesh.
 *
 * This enum structure defines the possible locations where a fragment can be relative to the cut-mesh. Note that the labels of 'above' or 'below' here are defined with-respect-to the winding-order (and hence, normal orientation) of the cut-mesh.
 */
typedef enum McFragmentLocation {
    MC_FRAGMENT_LOCATION_ABOVE = 1 << 0, /**< Fragment is located above the cut-mesh. */
    MC_FRAGMENT_LOCATION_BELOW = 1 << 1, /**< Fragment is located below the cut-mesh. */
    MC_FRAGMENT_LOCATION_UNDEFINED = 1 << 2, /**< Fragment is located neither above nor below the cut-mesh. That is, it was produced due to a partial cut intersection. */
    MC_FRAGMENT_LOCATION_ALL = 0xFFFFFFFF /**< Wildcard (match all) . */
} McFragmentLocation;

/**
 * \enum McFragmentSealType
 * @brief Topological configurations of a fragment as defined with-respect-to hole-filling/sealing.
 *
 * This enum structure defines the possible configurations that a fragment will be in regarding the hole-filling process. Here, hole-filling refers to the stage/phase when holes produced by a cut are filled with a subset of polygons of the cut-mesh.
 */
typedef enum McFragmentSealType {
    MC_FRAGMENT_SEAL_TYPE_COMPLETE = 1 << 0, /**< Holes are completely sealed (watertight). */
    MC_FRAGMENT_SEAL_TYPE_NONE = 1 << 2, /**< Holes are not sealed (gaping hole). */
    MC_FRAGMENT_SEAL_TYPE_ALL = 0xFFFFFFFF /**< Wildcard (match all) . */
} McFragmentSealType;

/**
 * \enum McPatchLocation
 * @brief The possible geometrical locations of a patch as defined with-respect-to the source-mesh.
 *
 * This enum structure defines the possible locations where a patch can be relative to the source-mesh. Note that the labels of 'inside' or 'outside' here are defined with-respect-to the winding-order (and hence, normal orientation) of the source-mesh.
 */
typedef enum McPatchLocation {
    MC_PATCH_LOCATION_INSIDE = 1 << 0, /**< Patch is located on the interior of the source-mesh (used to seal holes). */
    MC_PATCH_LOCATION_OUTSIDE = 1 << 1, /**< Patch is located on the exterior of the source-mesh. Rather than hole-filling these patches seal from the outside so-as to extrude the cut.*/
    MC_PATCH_LOCATION_UNDEFINED = 1 << 2, /**< Patch is located neither on the interior nor exterior of the source-mesh. */
    MC_PATCH_LOCATION_ALL = 0xFFFFFFFF /**< Wildcard (match all) . */
} McPatchLocation;

/**
 * \enum McSeamOrigin
 * @brief The input mesh from which a seam is derived.
 *
 * This enum structure defines the possible origins of a seam connected component, which can be either the source-mesh or the cut-mesh.
 */
typedef enum McSeamOrigin {
    MC_SEAM_ORIGIN_SRCMESH = 1 << 0, /**< Seam connected component is from the input source-mesh. */
    MC_SEAM_ORIGIN_CUTMESH = 1 << 1, /**< Seam connected component is from the input cut-mesh. */
    MC_SEAM_ORIGIN_ALL = 0xFFFFFFFF /**< Wildcard (match all) . */
} McSeamOrigin;

/**
 * \enum McInputOrigin
 * @brief The user-provided input mesh from which an input connected component is derived.
 *
 * This enum structure defines the possible origins of an input connected component, which can be either the source-mesh or the cut-mesh.
 * Note: the number of elements (faces and vertices) in an input connected component will be the same [or greater] than the corresponding user-provided input mesh from which the respective connected component came from. The input connect component will contain more elements if MCUT detected an intersection configuration where one input mesh will create a hole in a face of the other input mesh but without severing it edges (and vice versa).
 */
typedef enum McInputOrigin {
    MC_INPUT_ORIGIN_SRCMESH = 1 << 0, /**< Input connected component from the input source mesh.*/
    MC_INPUT_ORIGIN_CUTMESH = 1 << 1, /**< Input connected component from the input cut mesh. */
    MC_INPUT_ORIGIN_ALL = 0xFFFFFFFF /**< Wildcard (match all) . */
} McInputOrigin;

/**
 * \enum McConnectedComponentData
 * @brief Data that can be queried about a connected component.
 *
 * This enum structure defines the different types of data that are associated with a connected component and can be queried from the API after a dispatch call.
 */
typedef enum McConnectedComponentData {
    // MC_CONNECTED_COMPONENT_DATA_VERTEX_COUNT = (1 << 0), /**< Number of vertices. */
    MC_CONNECTED_COMPONENT_DATA_VERTEX_FLOAT = (1 << 1), /**< List of vertex coordinates as an array of 32 bit floating-point numbers. */
    MC_CONNECTED_COMPONENT_DATA_VERTEX_DOUBLE = (1 << 2), /**< List of vertex coordinates as an array of 64 bit floating-point numbers. */
    MC_CONNECTED_COMPONENT_DATA_DISPATCH_PERTURBATION_VECTOR = (1 << 4), /**< Tuple of three real numbers represneting the vector that was used to nudge/perturb the source- and cut-mesh into general position to produce the respective connected component. This vector will represent the zero-vector if no perturbation was applied prior to the cutting operation that produced this connected component. */
    MC_CONNECTED_COMPONENT_DATA_FACE = (1 << 5), /**< List of faces as an array of indices. Each face can also be understood as a "planar straight line graph" (PSLG), which is a collection of vertices and segments that lie on the same plane. Segments are edges whose endpoints are vertices in the PSLG.*/
    MC_CONNECTED_COMPONENT_DATA_FACE_SIZE = (1 << 6), /**< List of face sizes (vertices per face) as an array. */
    // MC_CONNECTED_COMPONENT_DATA_EDGE_COUNT = (1 << 7), /**< Number of edges. */
    MC_CONNECTED_COMPONENT_DATA_EDGE = (1 << 8), /**< List of edges as an array of indices. */
    MC_CONNECTED_COMPONENT_DATA_TYPE = (1 << 9), /**< The type of a connected component (See also: ::McConnectedComponentType.). */
    MC_CONNECTED_COMPONENT_DATA_FRAGMENT_LOCATION = (1 << 10), /**< The location of a fragment connected component with respect to the cut mesh (See also: ::McFragmentLocation). */
    MC_CONNECTED_COMPONENT_DATA_PATCH_LOCATION = (1 << 11), /**< The location of a patch with respect to the source mesh (See also: ::McPatchLocation).*/
    MC_CONNECTED_COMPONENT_DATA_FRAGMENT_SEAL_TYPE = (1 << 12), /**< The Hole-filling configuration of a fragment connected component (See also: ::McFragmentSealType). */
    MC_CONNECTED_COMPONENT_DATA_SEAM_VERTEX = (1 << 13), /**< List of unordered vertices as an array of indices. These vertices are introduced as a result of the cut and represent the intersection seam/path/contour. */
    MC_CONNECTED_COMPONENT_DATA_ORIGIN = (1 << 14), /**< The input mesh (source- or cut-mesh) from which a "seam" is derived (See also: ::McSeamOrigin). */
    MC_CONNECTED_COMPONENT_DATA_VERTEX_MAP = (1 << 15), /**< List of a subset of vertex indices from one of the input meshes (source-mesh or the cut-mesh). Each value will be the index of an input mesh vertex or MC_UNDEFINED_VALUE. This index-value corresponds to the connected component vertex at the accessed index. The value at index 0 of the queried array is the index of the vertex in the original input mesh. In order to clearly distinguish indices of the cut mesh from those of the source mesh, this index value corresponds to a cut mesh vertex index if it is great-than-or-equal-to the number of source-mesh vertices. Intersection points are mapped to MC_UNDEFINED_VALUE. The input mesh (i.e. source- or cut-mesh) will be deduced by the user from the type of connected component with which the information is queried. The input connected component (source-mesh or cut-mesh) that is referred to must be one stored internally by MCUT (i.e. a connected component queried from the API via ::McInputOrigin), to ensure consistency with any modification done internally by MCUT. */
    MC_CONNECTED_COMPONENT_DATA_FACE_MAP = (1 << 16), /**< List of a subset of face indices from one of the input meshes (source-mesh or the cut-mesh). Each value will be the index of an input mesh face. This index-value corresponds to the connected component face at the accessed index. Example: the value at index 0 of the queried array is the index of the face in the original input mesh. Note that all faces are mapped to a defined value. In order to clearly distinguish indices of the cut mesh from those of the source mesh, an input-mesh face index value corresponds to a cut-mesh vertex-index if it is great-than-or-equal-to the number of source-mesh faces. The input connected component (source-mesh or cut-mesh) that is referred to must be one stored internally by MCUT (i.e. a connected component queried from the API via ::McInputOrigin), to ensure consistency with any modification done internally by MCUT. */
    // incidence and adjacency information
    MC_CONNECTED_COMPONENT_DATA_FACE_ADJACENT_FACE = (1 << 17), /**< List of adjacent faces (their indices) per face.*/
    MC_CONNECTED_COMPONENT_DATA_FACE_ADJACENT_FACE_SIZE = (1 << 18), /**< List of adjacent-face-list sizes (number of adjacent faces per face).*/
    MC_CONNECTED_COMPONENT_DATA_FACE_TRIANGULATION = (1 << 19), /**< List of 3*N triangulated face indices, where N is the number of triangles that are produced using a [Constrained] Delaunay triangulation. Such a triangulation is similar to a Delaunay triangulation, but each (non-triangulated) face segment is present as a single edge in the triangulation. A constrained Delaunay triangulation is not truly a Delaunay triangulation. Some of its triangles might not be Delaunay, but they are all constrained Delaunay. */
    MC_CONNECTED_COMPONENT_DATA_FACE_TRIANGULATION_MAP = (1 << 20), /**< List of a subset of face indices from one of the input meshes (source-mesh or the cut-mesh). Each value will be the index of an input mesh face. This index-value corresponds to the connected-component face at the accessed index. Example: the value at index 0 of the queried array is the index of the face in the original input mesh. Note that all triangulated-faces are mapped to a defined value. In order to clearly distinguish indices of the cut mesh from those of the source mesh, an input-mesh face index value corresponds to a cut-mesh vertex-index if it is great-than-or-equal-to the number of source-mesh faces. The input connected component (source-mesh or cut-mesh) that is referred to must be one stored internally by MCUT (i.e. a connected component queried from the API via ::McInputOrigin), to ensure consistency with any modification done internally by MCUT. */
    
} McConnectedComponentData;

/**
 * \enum McDebugSource
 * @brief Source of a debug log message.
 *
 * This enum structure defines the sources from which a message in a debug log may originate.
 */
typedef enum McDebugSource {
    MC_DEBUG_SOURCE_API = 1 << 0, /**< messages generated by usage of the MCUT API. */
    MC_DEBUG_SOURCE_KERNEL = 1 << 1, /**< messages generated by internal logic implementing the kernel, which is responsible for resolving mesh intersections and connectivity. */
    MC_DEBUG_SOURCE_FRONTEND = 1 << 2, /**< messages generated by internal logic implementing the front-end, which is responsible for pre- and post-processing of meshes and connected components. */
    MC_DEBUG_SOURCE_IGNORE = 1 << 3, /**<  This flag is passed to the API function ::mcDebugMessageControl (second parameter). Its effect is to ignore the instruction to enable/disable the corresponding variant of debug output w.r.t. being logged into the provided callback*/
    MC_DEBUG_SOURCE_ALL = 0xFFFFFFFF /**< Wildcard (match all) . */
} McDebugSource;

/**
 * \enum McDebugType
 * @brief Type of debug messages.
 *
 * This enum structure defines the types of debug a message relating to an error.
 */
typedef enum McDebugType {
    MC_DEBUG_TYPE_ERROR = 1 << 0, /**< Explicit error message.*/
    MC_DEBUG_TYPE_DEPRECATED_BEHAVIOR = 1 << 1, /**< Attempted use of deprecated features.*/
    MC_DEBUG_TYPE_OTHER = 1 << 2, /**< Other types of messages,.*/
    MC_DEBUG_TYPE_IGNORE = 1 << 3, /**<  This flag is passed to the API function ::mcDebugMessageControl (third parameter). Its effect is to ignore the instruction to enable/disable the corresponding variant of debug output w.r.t. being logged into the provided callback*/
    MC_DEBUG_TYPE_ALL = 0xFFFFFFFF /**< Wildcard (match all) . */
} McDebugType;

/**
 * \enum McDebugSeverity
 * @brief Severity levels of messages.
 *
 * This enum structure defines the different severities of error: low, medium or high severity messages.
 */
typedef enum McDebugSeverity {
    MC_DEBUG_SEVERITY_HIGH = 1 << 0, /**< All MCUT Errors, mesh conversion/parsing errors, or undefined behavior.*/
    MC_DEBUG_SEVERITY_MEDIUM = 1 << 1, /**< Major performance warnings, debugging warnings, or the use of deprecated functionality.*/
    MC_DEBUG_SEVERITY_LOW = 1 << 2, /**< Redundant state change, or unimportant undefined behavior.*/
    MC_DEBUG_SEVERITY_NOTIFICATION = 1 << 3, /**< Anything that isn't an error or performance issue.*/
    MC_DEBUG_SEVERITY_IGNORE = 1 << 3, /**<  This flag is passed to the API function ::mcDebugMessageControl (forth parameter). Its effect is to ignore the instruction to enable/disable the corresponding variant of debug output w.r.t. being logged into the provided callback*/
    MC_DEBUG_SEVERITY_ALL = 0xFFFFFFFF /**< Match all (wildcard).*/
} McDebugSeverity;

/**
 * \enum McContextCreationFlags
 * @brief Context creation flags.
 *
 * This enum structure defines the flags with which a context can be created.
 */
typedef enum McContextCreationFlags {
    MC_DEBUG = (1 << 0), /**< Enable debug mode (message logging etc.).*/
    MC_OUT_OF_ORDER_EXEC_MODE_ENABLE = (1 << 1), /**< Determines whether the commands queued in the context-queue are executed in-order or out-of-order. If set, the commands in the context-queue are executed out-of-order. Otherwise, commands are executed in-order..*/
    MC_PROFILING_ENABLE = (1 << 2) /**< Enable or disable profiling of commands in the context-queue. If set, the profiling of commands is enabled. Otherwise profiling of commands is disabled. See ::mcGetEventProfilingInfo for more information. */
} McContextCreationFlags;

/**
 * \enum McDispatchFlags
 * @brief Dispatch configuration flags.
 *
 * This enum structure defines the flags indicating MCUT is to interprete input data, and execute the cutting pipeline.
 */
typedef enum McDispatchFlags {
    MC_DISPATCH_VERTEX_ARRAY_FLOAT = (1 << 0), /**< Interpret the input mesh vertices as arrays of 32-bit floating-point numbers.*/
    MC_DISPATCH_VERTEX_ARRAY_DOUBLE = (1 << 1), /**< Interpret the input mesh vertices as arrays of 64-bit floating-point numbers.*/
    MC_DISPATCH_REQUIRE_THROUGH_CUTS = (1 << 2), /**< Require that all intersection paths/curves/contours partition the source-mesh into two (or more) fully disjoint parts. Otherwise, ::mcDispatch is a no-op. This flag enforces the requirement that only through-cuts are valid cuts i.e it disallows partial cuts. NOTE: This flag may not be used with ::MC_DISPATCH_FILTER_FRAGMENT_LOCATION_UNDEFINED.*/
    MC_DISPATCH_INCLUDE_VERTEX_MAP = (1 << 3), /**< Compute connected-component-to-input mesh vertex-id maps. See also: ::MC_CONNECTED_COMPONENT_DATA_VERTEX_MAP */
    MC_DISPATCH_INCLUDE_FACE_MAP = (1 << 4), /**< Compute connected-component-to-input mesh face-id maps. . See also: ::MC_CONNECTED_COMPONENT_DATA_FACE_MAP and :MC_CONNECTED_COMPONENT_DATA_FACE_TRIANGULATION_MAP*/
    //
    MC_DISPATCH_FILTER_FRAGMENT_LOCATION_ABOVE = (1 << 5), /**< Compute fragments that are above the cut-mesh.*/
    MC_DISPATCH_FILTER_FRAGMENT_LOCATION_BELOW = (1 << 6), /**< Compute fragments that are below the cut-mesh.*/
    MC_DISPATCH_FILTER_FRAGMENT_LOCATION_UNDEFINED = (1 << 7), /**< Compute fragments that are partially cut i.e. neither above nor below the cut-mesh. NOTE: This flag may not be used with ::MC_DISPATCH_REQUIRE_THROUGH_CUTS. */
    //
    MC_DISPATCH_FILTER_FRAGMENT_SEALING_INSIDE = (1 << 8), /**< Compute fragments that are fully sealed (hole-filled) on the interior.   */
    MC_DISPATCH_FILTER_FRAGMENT_SEALING_OUTSIDE = (1 << 9), /**< Compute fragments that are fully sealed (hole-filled) on the exterior.  */
    //
    MC_DISPATCH_FILTER_FRAGMENT_SEALING_NONE = (1 << 10), /**< Compute fragments that are not sealed (holes not filled).*/
    //
    MC_DISPATCH_FILTER_PATCH_INSIDE = (1 << 11), /**< Compute patches on the inside of the source mesh (those used to fill holes).*/
    MC_DISPATCH_FILTER_PATCH_OUTSIDE = (1 << 12), /**< Compute patches on the outside of the source mesh.*/
    //
    MC_DISPATCH_FILTER_SEAM_SRCMESH = (1 << 13), /**< Compute the seam which is the same as the source-mesh but with new edges placed along the cut path. Note: a seam from the source-mesh will only be computed if the dispatch operation computes a complete (through) cut.*/
    MC_DISPATCH_FILTER_SEAM_CUTMESH = (1 << 14), /**< Compute the seam which is the same as the cut-mesh but with new edges placed along the cut path. Note: a seam from the cut-mesh will only be computed if the dispatch operation computes a complete (through) cut.*/
    //
    MC_DISPATCH_FILTER_ALL = ( //
        MC_DISPATCH_FILTER_FRAGMENT_LOCATION_ABOVE | //
        MC_DISPATCH_FILTER_FRAGMENT_LOCATION_BELOW | //
        MC_DISPATCH_FILTER_FRAGMENT_LOCATION_UNDEFINED | //
        MC_DISPATCH_FILTER_FRAGMENT_SEALING_INSIDE | //
        MC_DISPATCH_FILTER_FRAGMENT_SEALING_OUTSIDE | //
        MC_DISPATCH_FILTER_FRAGMENT_SEALING_NONE | //
        MC_DISPATCH_FILTER_PATCH_INSIDE | //
        MC_DISPATCH_FILTER_PATCH_OUTSIDE | //
        MC_DISPATCH_FILTER_SEAM_SRCMESH | //
        MC_DISPATCH_FILTER_SEAM_CUTMESH), /**< Keep all connected components resulting from the dispatched cut. */
    /**
     * The following two flags allow MCUT to perturb the cut-mesh if the inputs are found not to be in general position.
     *
     * MCUT is formulated for inputs in general position. Here the notion of general position is defined with respect 
     * to the orientation predicate (as evaluated on the intersecting polygons). Thus, a set of points is in general 
     * position if no three points are collinear and also no four points are coplanar.
     * 
     * MCUT uses the "MC_DISPATCH_ENFORCE_GENERAL_POSITION.." flags to inform of when to use perturbation (of the
     * cut-mesh) so as to bring the input into general position. In such cases, the idea is to solve the cutting
     * problem not on the given input, but on a nearby input. The nearby input is obtained by perturbing the given
     * input. The perturbed input will then be in general position and, since it is near the original input, 
     * the result for the perturbed input will hopefully still be useful.  This is justified by the fact that 
     * the task of MCUT is not to decide whether the input is in general position but rather to make perturbation
     * on the input (if) necessary within the available precision of the computing device. 
     * 
     * HOW GENERAL POSITION IS ENFORCED
     * 
     * When the inputs are found _not_ to be in GP, MCUT will generate a pseudo-random 
     * 3d vector "p" representing a translation that will be applied to the cut-mesh. 
     * 
     * A component "i" of "p" is computed as "p[i] = r() * c", where "r()" is a 
     * function returning a random variable from a uniform distribution on the 
     * interval [-1.0, 1.0), and "c" is a scalar computed from the general position 
     * enforcement constant (see ::MC_CONTEXT_GENERAL_POSITION_ENFORCEMENT_CONSTANT).
     * */

    MC_DISPATCH_ENFORCE_GENERAL_POSITION = (1 << 15), /**< Enforce general position such that the variable "c" (see detailed note above) is computed as the multiplication of the current general position enforcement constant (of current MCUT context) and the diagonal length of the bounding box of the cut-mesh. So this uses a relative perturbation of the cut-mesh based on its scale (see also ::MC_CONTEXT_GENERAL_POSITION_ENFORCEMENT_CONSTANT). */
    MC_DISPATCH_ENFORCE_GENERAL_POSITION_ABSOLUTE= (1 << 16), /**< Enforce general position such that the variable "c" (see detailed note above) is the current general position enforcement constant (of current MCUT context). So this uses an absolute perturbation of the cut-mesh based on the stored constant (see also ::MC_CONTEXT_GENERAL_POSITION_ENFORCEMENT_CONSTANT). */
} McDispatchFlags;

/**
 * \enum McEventOperationExevStatus
 * @brief Flags for querying the operation status.
 *
 * This enum structure defines the flags which are used for querying the execution status of an operation associated with an event.
 */
typedef enum McEventCommandExecStatus {
    MC_SUBMITTED = 1 << 0, /**< enqueued operation has been submitted by the client thread to the internal "device". */
    MC_RUNNING = 1 << 1, /**< Operation is currently running. */
    MC_COMPLETE = 1 << 2 /**< The operation has completed. */
} McEventCommandExecStatus;

/**
 * \enum McCommandType
 * @brief Flags for identifying event commands.
 *
 * This enum structure defines the flags which are used for identifying the MCUT command associated with an event.
 */
typedef enum McCommandType {
    MC_COMMAND_DISPATCH = 1 << 0, /**< From McEnqueueDispatch. */
    MC_COMMAND_GET_CONNECTED_COMPONENTS = 1 << 1, /**< From McEnqueueGetConnectedComponents. */
    MC_COMMAND_GET_CONNECTED_COMPONENT_DATA = 1 << 2, /**< From McEnqueueGetConnectedComponentData. */
    MC_COMMAND_USER = 1 << 3, /**< From user application. */
    MC_COMMAND_UKNOWN
} McCommandType;

/**
 * \enum McConnectedComponentFaceWindingOrder
 * @brief Flags for specifying the state used to determine winding order of queried faces.
 *
 * This enum structure defines the flags which are used for identifying the winding order that is used to orient the vertex indices defining the faces of connected components.
 */
typedef enum McConnectedComponentFaceWindingOrder {
    MC_CONNECTED_COMPONENT_FACE_WINDING_ORDER_AS_GIVEN = 1 << 0, /**< Define the order of face-vertex indices using the orientation implied by the input meshes. */
    MC_CONNECTED_COMPONENT_FACE_WINDING_ORDER_REVERSED = 1 << 1, /**< Define the order of face-vertex indices using the reversed orientation implied by the input meshes. */
} McConnectedComponentFaceWindingOrder;

/**
 * \enum McQueryFlags
 * @brief Flags for querying fixed API state.
 *
 * This enum structure defines the flags which are used to query for specific information about the state of the API and/or a given context.
 */
typedef enum McQueryFlags {
    MC_CONTEXT_FLAGS = 1 << 0, /**< Flags used to create a context.*/
    MC_DONT_CARE = 1 << 1, /**< wildcard.*/
    MC_EVENT_RUNTIME_EXECUTION_STATUS = 1 << 2, /**< Error/status code associated with the runtime of the asynchronous/non-blocking part of the associated task. See also ::McResult */
    MC_EVENT_TIMESTAMP_SUBMIT = 1 << 3, /**< An unsigned 64-bit value that describes the current internal time counter in nanoseconds when the MCUT API function identified by event that has been enqueued is submitted by the internal scheduler for execution.*/
    MC_EVENT_TIMESTAMP_START = 1 << 4, /**< An unsigned 64-bit value that describes the current internal time counter in nanoseconds when the MCUT API function identified by event starts execution.*/
    MC_EVENT_TIMESTAMP_END = 1 << 5, /**< An unsigned 64-bit value that describes the current internal time counter in nanoseconds when the MCUT API function identified by event has finished execution. */
    MC_EVENT_COMMAND_EXECUTION_STATUS = 1 << 6, /**< the execution status of the command identified by event. See also ::McEventCommandExecStatus */
    MC_EVENT_CONTEXT = 1 << 7, /**< The context associated with event. */
    MC_EVENT_COMMAND_TYPE = 1 << 8, /**< The command associated with event. Can be one of the values in :: */
    MC_CONTEXT_MAX_DEBUG_MESSAGE_LENGTH = 1 << 9, /**< The maximum length of a single message return from ::mcGetDebugMessageLog */
    MC_CONTEXT_GENERAL_POSITION_ENFORCEMENT_CONSTANT = 1 << 10, /**< A constant small real number representing the amount by which to perturb the cut-mesh when two intersecting polygon are found to not be in general position. */
    MC_CONTEXT_GENERAL_POSITION_ENFORCEMENT_ATTEMPTS = 1<<11, /**< The number of times that a dispatch operation will attempt to perturb the cut-mesh if the input meshes are found to not be in general position.*/
    MC_CONTEXT_CONNECTED_COMPONENT_FACE_WINDING_ORDER = 1<<12 /**< The winding order that is used when specifying vertex indices that define the faces of connected components. */
} McQueryFlags;

/**
 *
 * @brief Debug callback function signature type.
 *
 * The callback function should have this prototype (in C), or be otherwise compatible with such a prototype.
 */
typedef void(MCAPI_PTR* pfn_mcDebugOutput_CALLBACK)(
    McDebugSource source,
    McDebugType type,
    unsigned int id,
    McDebugSeverity severity,
    size_t length,
    const char* message,
    const McVoid* userParam);

/**
 *
 * @brief Event callback function signature type.
 *
 * The callback function should have this prototype (in C), or be otherwise compatible with such a prototype.
 */
typedef void(MCAPI_PTR* pfn_McEvent_CALLBACK)(McEvent event, McVoid* data);

/** @brief Create an MCUT context.
 *
 * This method creates a context object, which is a handle used by a client application to control the API state and access data.
 *
 * @param [out] pContext a pointer to the allocated context handle
 * @param [in] flags bitfield containing the context creation flags
 *
 * An example of usage:
 * @code
 * McContext myContext = MC_NULL_HANDLE;
 * McResult err = mcCreateContext(&myContext, MC_NULL_HANDLE);
 * if(err != MC_NO_ERROR)
 * {
 *  // deal with error
 * }
 * @endcode
 *
 * @return Error code.
 *
 * <b>Error codes</b>
 * - MC_NO_ERROR
 *   -# proper exit
 * - MC_INVALID_VALUE
 *   -# \p pContext is NULL
 *   -# Failure to allocate resources
 *   -# \p flags defines an invalid bitfield.
 */
extern MCAPI_ATTR McResult MCAPI_CALL mcCreateContext(
    McContext* pContext, McFlags flags);

/** @brief Create an MCUT context object with N helper threads.
 *
 * This method creates a context object with an additonal number of threads that
 * assist the internal scheduler with compute workloads. The returned a handle
 * shall be used by a client applications to control the API state and access data.
 *
 * Unless otherwise stated, MCUT runs asynchronously with the user application.
 * Non-blocking commands issued with a given context run on a conceptual "device",
 * which executes independently of the client application. This device is
 * associated with one logical thread by default. Two logical threads will be
 * associated with the device if MC_OUT_OF_ORDER_EXEC_MODE_ENABLE is provided as
 * a flag.
 *
 * Having one logical device thread means that MCUT commands shall execute in
 * the order that they are provided by a client application. And if two logical
 * threads are associated with the device then multiple API commands may also run
 * concurrently subject to their dependency list.
 *
 * Concurrent commands share the pool of N helper threads.
 *
 * @param [out] pContext a pointer to the allocated context handle
 * @param [in] flags bitfield containing the context creation flags
 * @param [in] helperThreadCount Number of helper-threads to assist device-threads with parallel work.
 *
 * An example of usage:
 * @code
 * McContext myContext = MC_NULL_HANDLE;
 * McResult err = mcCreateContextWithHelpers(&myContext, MC_OUT_OF_ORDER_EXEC_MODE_ENABLE, 2);
 * if(err != MC_NO_ERROR)
 * {
 *  // deal with error
 * }
 * @endcode
 *
 * @return Error code.
 *
 * <b>Error codes</b>
 * - MC_NO_ERROR
 *   -# proper exit
 * - MC_INVALID_VALUE
 *   -# \p pContext is NULL
 *   -# Failure to allocate resources
 *   -# \p flags defines an invalid bitfield.
 */
extern MCAPI_ATTR McResult MCAPI_CALL mcCreateContextWithHelpers(
    McContext* pOutContext, McFlags contextFlags, uint32_t helperThreadCount);

/** @brief Specify a callback to receive debugging messages from the MCUT library.
 *
 * ::mcDebugMessageCallback sets the current debug output callback function to the function whose address is
 * given in callback.
 *
 * This function is defined to have the same calling convention as the MCUT API functions. In most cases
 * this is defined as MCAPI_ATTR, although it will vary depending on platform, language and compiler.
 *
 * Each time a debug message is generated the debug callback function will be invoked with source, type,
 * and severity associated with the message, and length set to the length of debug message whose
 * character string is in the array pointed to by message userParam will be set to the value passed in
 * the userParam parameter to the most recent call to mcDebugMessageCallback.
 *
 * @param[in] context The context handle that was created by a previous call to mcCreateContext.
 * @param[in] cb The address of a callback function that will be called when a debug message is generated.
 * @param[in] userParam A user supplied pointer that will be passed on each invocation of callback.
 *
 * An example of usage:
 * @code
 * // define my callback (with type pfn_mcDebugOutput_CALLBACK)
 * void mcDebugOutput(McDebugSource source,   McDebugType type, unsigned int id, McDebugSeverity severity,size_t length, const char* message,const McVoid* userParam)
 * {
 *  // do stuff
 * }
 *
 * // ...
 *
 * McVoid* someData = NULL;
 * McResult err = mcDebugMessageCallback(myContext, mcDebugOutput, someData);
 * if(err != MC_NO_ERROR)
 * {
 *  // deal with error
 * }
 * @endcode
 *
 * @return Error code.
 *
 * <b>Error codes</b>
 * - MC_NO_ERROR
 *   -# proper exit
 * - MC_INVALID_VALUE
 *   -# \p pContext is NULL or \p pContext is not an existing context.
 *   -# \p cb is NULL.
 *
 */
extern MCAPI_ATTR McResult MCAPI_CALL mcDebugMessageCallback(
    McContext context,
    pfn_mcDebugOutput_CALLBACK cb,
    const McVoid* userParam);

/**
 * @brief Returns messages stored in an internal log.
 * 
 * @param context The context handle that was created by a previous call to @see ::mcCreateContext.
 * @param count The number of debug messages to retrieve from the log.
 * @param bufSize The size of the buffer whose address is given by \p messageLog.
 * @param sources The address of an array of variables to receive the sources of the retrieved messages.
 * @param types The address of an array of variables to receive the types of the retrieved messages.
 * @param severities The address of an array of variables to receive the severites of the retrieved messages.
 * @param lengths The address of an array of variables to receive the lengths of the received messages.
 * @param messageLog The address of an array of variables to receive the lengths of the received messages.. 
 * 
 * mcGetDebugMessageLog retrieves messages from the debug message log. A maximum 
 * of \p count messages are retrieved from the log. If \p sources is not NULL then 
 * the source of each message is written into up to count elements of the array. 
 * If \p types is not NULL then the type of each message is written into up to 
 * count elements of the array. If \p severities is not NULL then the severity 
 * of each message is written into up to count elements of the array. If \p lengths 
 * is not NULL then the length of each message is written into up to count 
 * elements of the array.
 * 
 * \p messageLog specifies the address of a character array into which the debug 
 * messages will be written. Each message will be concatenated onto the array 
 * starting at the first element of \p messageLog. \p bufSize specifies the size 
 * of the array \p messageLog. If a message will not fit into the remaining space 
 * in \p messageLog then the function terminates and returns the number of messages 
 * written so far, which may be zero.
 * 
 * If ::mcGetDebugMessageLog returns zero then no messages are present in the 
 * debug log, or there was not enough space in \p messageLog to retrieve the 
 * first message in the queue. If \p messageLog is NULL then no messages are 
 * written and the value of \p bufSize is ignored..
 * 
 * Here is an example of code that can get the first N messages:
 * 
 * @code {.c++}
 * McResult GetFirstNMessages(McContext context, McUint32 numMsgs)
 * {
 *      McSize maxMsgLen = 0;
 *      mcGetInfo(MC_CONTEXT_MAX_DEBUG_MESSAGE_LENGTH, &maxMsgLen);
 * 	    std::vector<McChar> msgData(numMsgs * maxMsgLen);
 *      std::vector<McDebugSource> sources(numMsgs);
 *      std::vector<McDebugType> types(numMsgs);
 *      std::vector<McDebugSeverity> severities(numMsgs);
 *      std::vector<McSize> lengths(numMsgs);
 *      McUint32 numFound;
 * 
 *      McResult err = mcGetDebugMessageLog(context, numMsgs, numMsgs, &sources[0], &types[0], &severities[0], &lengths[0], &msgData[0], &numFound);
 *      
 *      if(err != MC_NO_ERROR)
 *      {
 *          // deal with error ...
 *      }
 * 
 *      sources.resize(numFound);
 *      types.resize(numFound);
 *      severities.resize(numFound);
 *      ids.resize(numFound);
 *      lengths.resize(numFound);
 *      std::vector<std::string> messages;
 *      messages.reserve(numFound);
 * 
 *      std::vector<McChar>::iterator currPos = msgData.begin();
 *      
 *      for(size_t msg = 0; msg < lengths.size(); ++msg)
 *      {
 *          messages.push_back(std::string(currPos, currPos + lengths[msg] - 1));
 *          currPos = currPos + lengths[msg];
 *      }
 *  }
 * @endcode
 * 
 * @return error code
 */
extern MCAPI_ATTR McResult MCAPI_CALL mcGetDebugMessageLog(
    McContext context,  
    McUint32 count, McSize bufSize,
    McDebugSource *sources, McDebugType *types, McDebugSeverity *severities,
    McSize *lengths, McChar *messageLog, McUint32* numFetched);

/**
 * Control the reporting of debug messages in a debug context.
 *
 * @param[in] context The context handle that was created by a previous call to @see ::mcCreateContext.
 * @param[in] source The source of debug messages to enable or disable.
 * @param[in] type The type of debug messages to enable or disable.
 * @param[in] severity The severity of debug messages to enable or disable.
 * @param[in] enabled A Boolean flag determining whether the selected messages should be enabled or disabled.
 *
 * ::mcDebugMessageControl controls the reporting of debug messages generated by a debug context. The parameters
 * source, type and severity form a filter to select messages from the pool of potential messages generated by
 * the MCUT library.
 *
 * \p source may be #MC_DEBUG_SOURCE_API, #MC_DEBUG_SOURCE_KERNEL to select messages
 * generated by usage of the MCUT API, the MCUT kernel or by the input, respectively. It may also take the
 * value #MC_DEBUG_SOURCE_ALL. If source is not #MC_DEBUG_SOURCE_ALL then only messages whose source matches
 * source will be referenced.
 *
 * \p type may be one of #MC_DEBUG_TYPE_ERROR, #MC_DEBUG_TYPE_DEPRECATED_BEHAVIOR, or #MC_DEBUG_TYPE_OTHER to indicate
 * the type of messages describing MCUT errors, attempted use of deprecated features, and other types of messages,
 * respectively. It may also take the value #MC_DONT_CARE. If type is not #MC_DEBUG_TYPE_ALL then only messages whose
 * type matches type will be referenced.
 *
 * \p severity may be one of #MC_DEBUG_SEVERITY_LOW, #MC_DEBUG_SEVERITY_MEDIUM, or #MC_DEBUG_SEVERITY_HIGH to
 * select messages of low, medium or high severity messages or to #MC_DEBUG_SEVERITY_NOTIFICATION for notifications.
 * It may also take the value #MC_DEBUG_SEVERITY_ALL. If severity is not #MC_DEBUG_SEVERITY_ALL then only
 * messages whose severity matches severity will be referenced.
 *
 * If \p enabled is true then messages that match the filter formed by source, type and severity are enabled.
 * Otherwise, those messages are disabled.
 *
 *
 * An example of usage:
 * @code
 * // ... typically after setting debug callback with ::mcDebugMessageCallback
 * McResult err = mcDebugMessageControl(myContext, MC_DEBUG_SOURCE_ALL, MC_DEBUG_TYPE_ALL, MC_DEBUG_SEVERITY_ALL, true);
 * if(err != MC_NO_ERROR)
 * {
 *  // deal with error
 * }
 * @endcode
 *
 * @return Error code.
 *
 * <b>Error codes</b>
 * - MC_NO_ERROR
 *   -# proper exit
 * - MC_INVALID_VALUE
 *   -# \p pContext is NULL or \p pContext is not an existing context.
 *   -# \p source is not a value define in ::McDebugSource.
 *   -# \p type is not a value define in ::McDebugType.
 *   -# \p severity is not a value define in ::McDebugSeverity.
 */
extern MCAPI_ATTR McResult MCAPI_CALL mcDebugMessageControl(
    McContext context,
    McDebugSource source,
    McDebugType type,
    McDebugSeverity severity,
    bool enabled);

/**
 * @brief To create a user event object, call the function
 *
 * @param event Event handle
 * @param context must be a valid MCUT context.
 *
 * User events allow applications to enqueue commands that wait on a user event
 * to finish before the command is executed by the MCUT engine
 *
 * mcCreateUserEvent returns a valid non-zero event object and return code is
 * set to MC_NO_ERROR if the user event object is created successfully.
 * Otherwise, it returns a NULL value with an error values.
 *
 * @return MCAPI_ATTR
 */
extern MCAPI_ATTR McResult MCAPI_CALL mcCreateUserEvent(
    McEvent* event,
    McContext context);

/**
 * @brief To set the execution status of a user event object, call the function
 *
 * @param event is a user event object created using mcCreateUserEvent.
 * @param execution_status specifies the new execution status to be set and can
 * be MC_​COMPLETE or an ::McResult value to indicate an error. Thus a negative
 * integer value causes all enqueued commands that wait on this user event to
 * be terminated. mcSetUserEventStatus can only be called once to change the
 * execution status of event.
 * 
 * If there are enqueued commands with user events in the event_wait_list 
 * argument of mcEnqueue* commands, the user must ensure that the status of 
 * these user events being waited on are set using mcSetUserEventStatus before 
 * any MCUT APIs that release MCUT objects except for event objects are called; 
 * otherwise the behavior is undefined.
 *
 */
extern MCAPI_ATTR McResult MCAPI_CALL mcSetUserEventStatus(
    McEvent event,
    McInt32 execution_status);

/**
 * @brief Returns information about the event object..
 *
 * @param[in] event Specifies the event object being queried..
 * @param[in] info Specifies the information to query. See::
 * @param[in] bytes Specifies the size in bytes of memory pointed to by `pMem`. This size must be >= size of the return type as described in the table below..
 * @param[in] pMem A pointer to memory where the appropriate result being queried is returned. If param_value is NULL, it is ignored.
 * @param[in] pNumBytes Returns the actual size in bytes of data copied to `pMem`. If `pNumBytes` is NULL, it is ignored.
 *
 * Using mcGetEventInfo to determine if a operation identified by event has finished
 * execution (i.e. MC_EVENT_COMMAND_EXECUTION_STATUS returns MC_COMPLETE) is not
 * a synchronization point. There are no guarantees that the memory objects being
 * modified by the operation associated with event will be visible to other
 * enqueued commands.
 *
 * @return Error code.
 *
 * <b>Error codes</b>
 * - ::MC_NO_ERROR
 *   -# proper exit
 * - ::MC_INVALID_VALUE
 *   -# \p event is not a valid object
 *   -# \p bytes is greater than zero but \p pMem is null.
 *   -# \p bytes is zero but \p pMem is not null.
 *   -# \p bytes is zero \p pMem is null and \p pNumBytes is null.
 *   -# \p bytes is incompatible with \p info.
 */
extern MCAPI_ATTR McResult MCAPI_CALL mcGetEventInfo(const McEvent event, McFlags info, McSize bytes, McVoid* pMem, McSize* pNumBytes);
/**
 * @brief Registers a user callback function.
 *
 * @param[in] event A valid event object returned by the MCUT API.
 * @param[in] pfn_notify The event callback function that can be registered by the application..
 * @param[in] user_data Will be passed as the user_data argument when pfn_notify is called. user_data can be NULL.
 *
 * The registered callback function will be called when the execution of the
 * command associated with event is complete.
 * Each call to clSetEventCallback registers the specified user callback function
 * and replaces any previously specified callback.
 * An enqueued callback shall be called before the event object is destroyed.
 * The callback must return promptly. The behavior of calling expensive system
 * routines, or calling blocking MCUT operations etc. in a callback is undefined.
 * It is the application's responsibility to ensure that the callback function is
 * thread-safe.
 *
 * An example of usage:
 * @code
 *  McResult err = mcSetEventCallback(ev, someFunctionPointer, NULL);
 *  if(err != MC_NO_ERROR)
 *  {
 *   // deal with error
 *  }
 * @endcode
 *
 * @return Error code.
 *
 * <b>Error codes</b>
 * - ::MC_NO_ERROR
 *   -# proper exit
 * - ::MC_INVALID_VALUE
 *   -# \p event is not a valid object
 *   -# \p eventCallback is NULL
 */
extern MCAPI_ATTR McResult MCAPI_CALL mcSetEventCallback(
    McEvent event,
    pfn_McEvent_CALLBACK pfn_notify,
    McVoid* user_data);

/**
 * @brief Execute a cutting operation with two meshes - the source mesh, and the cut mesh.
 *
 * @param[in] context The context handle that was created by a previous call to ::mcCreateContext.
 * @param[in] flags The flags indicating how to interprete input data and configure the execution.
 * @param[in] pSrcMeshVertices The array of vertex coordinates (i.e. in xyzxyzxyz... format) of the source mesh.
 * @param[in] pSrcMeshFaceIndices The array of vertex indices of the faces (polygons) in the source mesh.
 * @param[in] pSrcMeshFaceSizes The array of the sizes (in terms of number of vertex indices) of the faces in the source mesh.
 * @param[in] numSrcMeshVertices The number of vertices in the source mesh.
 * @param[in] numSrcMeshFaces The number of faces in the source mesh.
 * @param[in] pCutMeshVertices The array of vertex coordinates (i.e. in xyzxyzxyz... format) of the cut mesh.
 * @param[in] pCutMeshFaceIndices The array of vertex indices of the faces (polygons) in the cut mesh.
 * @param[in] pCutMeshFaceSizes The array of the sizes (in terms of number of vertex indices) of the faces in the cut mesh.
 * @param[in] numCutMeshVertices The number of vertices in the cut mesh.
 * @param[in] numCutMeshFaces The number of faces in the cut mesh.
 * @param[in] numEventsInWaitlist Number of events in the waitlist.
 * @param[in] pEventWaitList Events that need to complete before this particular command can be executed.
 * @param[out] pEvent Returns an event object that identifies this particular command and can be used to query or queue a wait for this particular command to complete.  pEvent can be NULL in which case it will not be possible for the application to query the status of this command or queue a wait for this command to complete. If the pEventWaitList and the event arguments are not NULL, the pEvent argument should not refer to an element of the pEventWaitList array.
 *
 * This function specifies the two mesh objects to operate on. The 'source mesh' is the mesh to be cut
 * (i.e. partitioned) along intersection paths prescribed by the 'cut mesh'.
 *
 * If pEventWaitList is NULL, then this particular command does not wait on any event to complete. If pEventWaitList
 * is NULL, numEventsInWaitlist must be 0. If pEventWaitList is not NULL, the list of events pointed to
 * by pEventWaitList must be valid and numEventsInWaitlist must be greater than 0. The events specified in
 * pEventWaitList act as synchronization points. The memory associated with pEventWaitList can be reused or
 * freed after the function returns.
 *
 * An example of usage:
 * @code
 *  McEvent event;
 *  //...
 *  McResult err = mcEnqueueDispatch(..., 0, NULL, &event);
 * if(err != MC_NO_ERROR)
 * {
 *  // deal with error...
 * }
 *
 * err = mcWaitForEvents(1, &event);
 * if(err != MC_NO_ERROR)
 * {
 *  // deal with error...
 * }
 * @endcode
 *
 * @return Error code.
 *
 * <b>Error codes</b>
 * - ::MC_NO_ERROR
 *   -# proper exit
 * - ::MC_INVALID_VALUE
 *   -# \p pContext is NULL or \p pContext is not an existing context.
 *   -# \p flags contains an invalid value.
 *   -# A vertex index in \p pSrcMeshFaceIndices or \p pCutMeshFaceIndices is out of bounds.
 *   -# Invalid face/polygon definition (vertex list) implying non-manifold mesh \p pSrcMeshFaceIndices or \p pCutMeshFaceIndices is out of bounds.
 *   -# The MC_DISPATCH_VERTEX_ARRAY_... value has not been specified in \p flags
 *   -# An input mesh contains multiple connected components.
 *   -# \p pSrcMeshVertices is NULL.
 *   -# \p pSrcMeshFaceIndices is NULL.
 *   -# \p pSrcMeshFaceSizes is NULL.
 *   -# \p numSrcMeshVertices is less than three.
 *   -# \p numSrcMeshFaces is less than one.
 *   -# \p pCutMeshVertices is NULL.
 *   -# \p pCutMeshFaceIndices is NULL.
 *   -# \p pCutMeshFaceSizes is NULL.
 *   -# \p numCutMeshVertices is less than three.
 *   -# \p numCutMeshFaces is less than one.
 *   -# \p numEventsInWaitlist Number of events in the waitlist.
 *   -# \p pEventWaitList events that need to complete before this particular command can be executed
 *   -# ::MC_DISPATCH_ENFORCE_GENERAL_POSITION or ::MC_DISPATCH_ENFORCE_GENERAL_POSITION_ABSOLUTE is not set and: 1) Found two intersecting edges between the source-mesh and the cut-mesh and/or 2) An intersection test between a face and an edge failed because an edge vertex only touches (but does not penetrate) the face, and/or 3) One or more source-mesh vertices are colocated with one or more cut-mesh vertices.
 * - ::MC_OUT_OF_MEMORY
 *   -# Insufficient memory to perform operation.
 */
extern MCAPI_ATTR McResult MCAPI_CALL mcEnqueueDispatch(
    const McContext context,
    McFlags dispatchFlags,
    const McVoid* pSrcMeshVertices,
    const uint32_t* pSrcMeshFaceIndices,
    const uint32_t* pSrcMeshFaceSizes,
    uint32_t numSrcMeshVertices,
    uint32_t numSrcMeshFaces,
    const McVoid* pCutMeshVertices,
    const uint32_t* pCutMeshFaceIndices,
    const uint32_t* pCutMeshFaceSizes,
    uint32_t numCutMeshVertices,
    uint32_t numCutMeshFaces,
    uint32_t numEventsInWaitlist,
    const McEvent* pEventWaitList,
    McEvent* pEvent);

/**
 * @brief Blocking version of ::mcEnqueueDispatch.
 */
extern MCAPI_ATTR McResult MCAPI_CALL mcDispatch(
    McContext context,
    McFlags flags,
    const McVoid* pSrcMeshVertices,
    const uint32_t* pSrcMeshFaceIndices,
    const uint32_t* pSrcMeshFaceSizes,
    uint32_t numSrcMeshVertices,
    uint32_t numSrcMeshFaces,
    const McVoid* pCutMeshVertices,
    const uint32_t* pCutMeshFaceIndices,
    const uint32_t* pCutMeshFaceSizes,
    uint32_t numCutMeshVertices,
    uint32_t numCutMeshFaces);

extern MCAPI_ATTR McResult MCAPI_CALL mcEnqueueDispatchPlanarSection(
    const McContext context,
    McFlags dispatchFlags,
    const McVoid* pSrcMeshVertices,
    const uint32_t* pSrcMeshFaceIndices,
    const uint32_t* pSrcMeshFaceSizes,
    uint32_t numSrcMeshVertices,
    uint32_t numSrcMeshFaces,
    const McDouble* pNormalVector,
    const McDouble sectionOffset,
    uint32_t numEventsInWaitlist,
    const McEvent* pEventWaitList,
    McEvent* pEvent);

/**
 * @brief Return the value of a selected parameter.
 *
 * @param[in] context The context handle that was created by a previous call to ::mcCreateContext.
 * @param[in] info Information object being queried. ::McQueryFlags
 * @param[in] bytes Size in bytes of memory pointed to by \p pMem. This size must be great than or equal to the return type size of data type queried.
 * @param[out] pMem Pointer to memory where the appropriate result being queried is returned. If \p pMem is NULL, it is ignored.
 * @param[out] pNumBytes returns the actual size in bytes of data being queried by info. If \p pNumBytes is NULL, it is ignored.
 *
 *
 * An example of usage:
 * @code
 * McSize numBytes = 0;
 * McFlags contextFlags;
 * McResult err =  mcGetInfo(context, MC_CONTEXT_FLAGS, 0, nullptr, &numBytes);
 * if(err != MC_NO_ERROR)
 * {
 *  // deal with error
 * }
 *
 *   err = mcGetInfo(context, MC_CONTEXT_FLAGS, numBytes, &contextFlags, nullptr);
 * if(err != MC_NO_ERROR)
 * {
 *  // deal with error
 * }
 * @endcode
 * @return Error code.
 *
 * <b>Error codes</b>
 * - MC_NO_ERROR
 *   -# proper exit
 * - MC_INVALID_VALUE
 *   -# \p pContext is NULL or \p pContext is not an existing context.
 *   -# \p bytes is greater than the returned size of data type queried
 */
extern MCAPI_ATTR McResult MCAPI_CALL mcGetInfo(
    const McContext context,
    McFlags info,
    McSize bytes,
    McVoid* pMem,
    McSize* pNumBytes);

/**
 * @brief Set the value of a selected parameter of a context.
 *
 * @param[in] context The context handle that was created by a previous call to ::mcCreateContext.
 * @param[in] info Information being set. ::McQueryFlags
 * @param[in] bytes Size in bytes of memory pointed to by \p pMem.
 * @param[out] pMem Pointer to memory from where the appropriate result being copied.
 * 
 * This function effectively sets state variables of a context. All API functions using the respective context shall be effected by this state.
 *
 * An example of usage:
 * @code
 * McDouble epsilon = 1e-4;
 * McResult err =  mcBindState(context, MC_CONTEXT_GENERAL_POSITION_ENFORCEMENT_CONSTANT, sizeof(McDouble), &epsilon);
 * if(err != MC_NO_ERROR)
 * {
 *  // deal with error
 * }
 * @endcode
 * @return Error code.
 *
 * <b>Error codes</b>
 * - MC_NO_ERROR
 *   -# proper exit
 * - MC_INVALID_VALUE
 *   -# \p pContext is NULL or \p pContext is not an existing context.
 *   -# \p stateInfo is not an accepted flag.
 *   -# \p bytes is 0
 *   -# \p pMem is NULL  
 */
extern MCAPI_ATTR McResult MCAPI_CALL mcBindState(
    const McContext context,
    McFlags stateInfo,
    McSize bytes,
    const McVoid* pMem);

/**
 * @brief Query the connected components available in a context.
 *
 * This function will return an array of connected components matching the given description of flags.
 *
 * @param[in] context The context handle
 * @param[in] connectedComponentType The type(s) of connected component sought. See also ::McConnectedComponentType.
 * @param[in] numEntries The number of ::McConnectedComponent entries that can be added to \p pConnComps. If \p pConnComps is not NULL, \p numEntries must be the number of elements in \p pConnComps.
 * @param[out] pConnComps Returns a list of connected components found. The ::McConnectedComponentType values returned in \p pConnComps can be used
 * to identify a specific connected component. If \p pConnComps is NULL, this argument is ignored. The number of connected components returned
 * is the minimum of the value specified by \p numEntries or the number of connected components whose type matches \p connectedComponentType.
 * @param[out] numConnComps Returns the number of connected components available that match \p connectedComponentType. If \p numConnComps is NULL,
 * this argument is ignored.
 * @param[in] numEventsInWaitlist Number of events in the waitlist.
 * @param[in] pEventWaitList Events that need to complete before this particular command can be executed.
 * @param[out] pEvent Returns an event object that identifies this particular command and can be used to query or queue a wait for this particular command to complete.  pEvent can be NULL in which case it will not be possible for the application to query the status of this command or queue a wait for this command to complete. If the pEventWaitList and the event arguments are not NULL, the pEvent argument should not refer to an element of the pEventWaitList array.
 *
 * If pEventWaitList is NULL, then this particular command does not wait on any event to complete. If pEventWaitList
 * is NULL, numEventsInWaitlist must be 0. If pEventWaitList is not NULL, the list of events pointed to
 * by pEventWaitList must be valid and numEventsInWaitlist must be greater than 0. The events specified in
 * pEventWaitList act as synchronization points. The memory associated with pEventWaitList can be reused or
 * freed after the function returns.
 *
 * An example of usage:
 * @code
 * uint32_t numConnComps = 0;
 * McConnectedComponent* pConnComps;
 * McEvent ev = MC_NULL_HANDLE;
 * McResult err =  err = mcGetConnectedComponents(myContext, MC_CONNECTED_COMPONENT_TYPE_ALL, 0, NULL, &numConnComps, 0, NULL, &ev);
 * if(err != MC_NO_ERROR)
 * {
 *  // deal with error
 * }
 *
 * if (numConnComps == 0) {
 *    // ...
 * }
 *
 * pConnComps = (McConnectedComponent*)malloc(sizeof(McConnectedComponent) * numConnComps);
 *
 * err = mcGetConnectedComponents(myContext, MC_CONNECTED_COMPONENT_TYPE_ALL, numConnComps, pConnComps, NULL, 1, &ev, NULL);
 * if(err != MC_NO_ERROR)
 * {
 *  // deal with error
 * }
 * ...
 * @endcode
 *
 * @return Error code.
 *
 * <b>Error codes</b>
 * - MC_NO_ERROR
 *   -# proper exit
 * - MC_INVALID_VALUE
 *   -# \p pContext is NULL or \p pContext is not an existing context.
 *   -# \p connectedComponentType is not a value in ::McConnectedComponentType.
 *   -# \p numConnComps and \p pConnComps are both NULL.
 *   -# \p numConnComps is zero and \p pConnComps is not NULL.
 *   -# \p
 */
MCAPI_ATTR McResult MCAPI_CALL mcEnqueueGetConnectedComponents(
    const McContext context,
    const McConnectedComponentType connectedComponentType,
    const uint32_t numEntries,
    McConnectedComponent* pConnComps,
    uint32_t* numConnComps,
    uint32_t numEventsInWaitlist,
    const McEvent* pEventWaitList,
    McEvent* pEvent);

/**
 * @brief Blocking version of ::mcEnqueueGetConnectedComponents.
 */
extern MCAPI_ATTR McResult MCAPI_CALL mcGetConnectedComponents(
    const McContext context,
    const McConnectedComponentType connectedComponentType,
    const uint32_t numEntries,
    McConnectedComponent* pConnComps,
    uint32_t* numConnComps);

/**
 * @brief Query specific information about a connected component.
 *
 * @param[in] context The context handle that was created by a previous call to ::mcCreateContext.
 * @param[in] connCompId A connected component returned by ::mcGetConnectedComponents whose data is to be read.
 * @param[in] flags An enumeration constant that identifies the connected component information being queried.
 * @param[in] bytes Specifies the size in bytes of memory pointed to by \p flags.
 * @param[out] pMem A pointer to memory location where appropriate values for a given \p flags will be returned. If \p pMem is NULL, it is ignored.
 * @param[out] pNumBytes Returns the actual size in bytes of data being queried by \p flags. If \p pNumBytes is NULL, it is ignored.
 *
 * The connected component queries described in the ::McConnectedComponentData should return the same information for a connected component returned by ::mcGetConnectedComponents.
 * If pEventWaitList is NULL, then this particular command does not wait on any event to complete. If pEventWaitList
 * is NULL, numEventsInWaitlist must be 0. If pEventWaitList is not NULL, the list of events pointed to
 * by pEventWaitList must be valid and numEventsInWaitlist must be greater than 0. The events specified in
 * pEventWaitList act as synchronization points. The memory associated with pEventWaitList can be reused or
 * freed after the function returns.
 *
 * An example of usage:
 * @code
 * McSize numBytes = 0;
 * McEvent bytesQueryEvent=MC_NULL_HANDLE;
 * McEvent ev = MC_NULL_HANDLE;
 * McResult err = mcEnqueueGetConnectedComponentData(myContext,  connCompHandle, MC_CONNECTED_COMPONENT_DATA_VERTEX_DOUBLE, 0, NULL, &numBytes, 0, NULL, &bytesQueryEvent, 0, NULL, &ev);
 * if(err != MC_NO_ERROR)
 * {
 *  // deal with error
 * }
 *
 * double* pVertices = (double*)malloc(numBytes);
 * // this operation shall happen AFTER the preceding operation associated with "bytesQueryEvent".
 * err = mcEnqueueGetConnectedComponentData(context, connCompHandle, MC_CONNECTED_COMPONENT_DATA_VERTEX_DOUBLE, numBytes, (McVoid*)pVertices, NULL, 1, &bytesQueryEvent, NULL, 1, &ev, NULL);
 * if(err != MC_NO_ERROR)
 * {
 *  // deal with error
 * }
 * @endcode
 *
 * @return Error code.
 *
 * <b>Error codes</b>
 * - MC_NO_ERROR
 *   -# proper exit
 * - MC_INVALID_VALUE
 *   -# \p pContext is NULL or \p pContext is not an existing context.
 *   -# \p connectedComponentType is not a value in ::McConnectedComponentType.
 *   -# \p pMem and \p pNumBytes are both NULL (or not NULL).
 *   -# \p bytes is zero and \p pMem is not NULL.
 *   -# \p flag is MC_CONNECTED_COMPONENT_DATA_VERTEX_MAP when \p context dispatch flags did not include flag MC_DISPATCH_INCLUDE_VERTEX_MAP
 *   -# \p flag is MC_CONNECTED_COMPONENT_DATA_FACE_MAP when \p context dispatch flags did not include flag MC_DISPATCH_INCLUDE_FACE_MAP
 *   -# \p numEventsInWaitlist Number of events in the waitlist.
 *   -# \p pEventWaitList events that need to complete before this particular command can be executed
 */
extern MCAPI_ATTR McResult MCAPI_CALL mcEnqueueGetConnectedComponentData(
    const McContext context,
    const McConnectedComponent connCompId,
    McFlags queryFlags,
    McSize bytes,
    McVoid* pMem,
    McSize* pNumBytes,
    uint32_t numEventsInWaitlist,
    const McEvent* pEventWaitList,
    McEvent* pEvent);

/**
 * @brief Blocking version of ::mcEnqueueGetConnectedComponents.
 */
extern MCAPI_ATTR McResult MCAPI_CALL mcGetConnectedComponentData(
    const McContext context,
    const McConnectedComponent connCompId,
    McFlags flags,
    McSize bytes,
    McVoid* pMem,
    McSize* pNumBytes);

/**
 * @brief Waits on the user thread for commands identified by event objects to complete.
 *
 * @param[in] numEventsInWaitlist Number of events to wait for
 * @param[in] pEventWaitList  List of events to wait for
 *
 * Waits on the user thread for commands identified by event objects in \p pEventWaitList to complete.
 * The events specified in \p pEventWaitList act as synchronization points.
 *
 * @return Error code.
 *   -# proper exit
 * - MC_INVALID_VALUE
 *   -# \p numEventsInWaitlist is greater than 0 and \p pEventWaitList is NULL (and vice versa).
 *   -# If an event object in \p pEventWaitList is not a valid event object.
 */
extern MCAPI_ATTR McResult MCAPI_CALL mcWaitForEvents(
    uint32_t numEventsInWaitlist,
    const McEvent* pEventWaitList);

/**
 * @brief Decrements the event reference count.
 *
 * @param numEvents Number of event objects to release
 * @param pEvents Array of event objects to release
 *
 * The event object is deleted once the reference count becomes zero, the specific
 * command identified by this event has completed (or terminated) and there are
 * no commands in the internal-queue of a context that require a wait for this
 * event to complete.
 *
 * @return Error code.
 *   -# proper exit
 * - MC_INVALID_VALUE
 *   -# \p numEvents is greater than 0 and \p pEvents is NULL (and vice versa).
 *   -# If an event object in \p pEvents is not a valid event object.
 */
extern MCAPI_ATTR McResult MCAPI_CALL mcReleaseEvents(
    uint32_t numEvents,
    const McEvent* pEvents);

/**
 * @brief To release the memory of a connected component, call this function.
 *
 * If \p numConnComps is zero and \p pConnComps is NULL, the memory of all connected components associated with the context is freed.
 *
 * @param[in] context The context handle that was created by a previous call to ::mcCreateContext.
 * @param[in] numConnComps Number of connected components in \p pConnComps whose memory to release.
 * @param[in] pConnComps The connected components whose memory will be released.
 *
 * An example of usage:
 * @code
 * McResult err = mcReleaseConnectedComponents(myContext, pConnComps, numConnComps);
 * // OR (delete all connected components in context)
 * //McResult err = mcReleaseConnectedComponents(myContext, NULL, 0);
 * if(err != MC_NO_ERROR)
 * {
 *  // deal with error
 * }
 * @endcode
 *
 * @return Error code.
 *
 * <b>Error codes</b>
 * - MC_NO_ERROR
 *   -# proper exit
 * - MC_INVALID_VALUE
 *   -# \p pContext is NULL or \p pContext is not an existing context.
 *   -# \p numConnComps is zero and \p pConnComps is not NULL (and vice versa).
 *
 */
extern MCAPI_ATTR McResult MCAPI_CALL mcReleaseConnectedComponents(
    const McContext context,
    uint32_t numConnComps,
    const McConnectedComponent* pConnComps);

/**
* @brief To release the memory of a context, call this function.
*
* This function ensures that all the state attached to context (such as unreleased connected components, and threads) are released, and the memory is deleted.

* @param[in] context The context handle that was created by a previous call to ::mcCreateContext.
*
*
 * An example of usage:
 * @code
 * McResult err = mcReleaseContext(myContext);
 * if(err != MC_NO_ERROR)
 * {
 *  // deal with error
 * }
 * @endcode
*
* @return Error code.
*
* <b>Error codes</b>
* - MC_NO_ERROR
*   -# proper exit
* - MC_INVALID_VALUE
*   -# \p pContext is NULL or \p pContext is not an existing context.
*/
extern MCAPI_ATTR McResult MCAPI_CALL mcReleaseContext(
    McContext context);

#ifdef __cplusplus
} // extern "C"
#endif // __cplusplus

#endif // #ifndef MCUT_API_H_
