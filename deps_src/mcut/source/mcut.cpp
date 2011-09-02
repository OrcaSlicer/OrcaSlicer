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

#include "mcut/mcut.h"

#include "mcut/internal/frontend.h"
#include "mcut/internal/timer.h"
#include "mcut/internal/utils.h"

#include <exception>
#include <stdexcept>

#if defined(MCUT_BUILD_WINDOWS)
#pragma warning(disable : 26812)
#endif

MCAPI_ATTR McResult MCAPI_CALL mcCreateContext(McContext* pOutContext, McFlags contextFlags)
{
    McResult return_value = McResult::MC_NO_ERROR;
    per_thread_api_log_str.clear();

    if (pOutContext == nullptr) {
        per_thread_api_log_str = "context ptr undef (NULL)";
        return_value = McResult::MC_INVALID_VALUE;
    } else {
        try {
            // no helper threads
            // only manager threads (2 managers if context is created with MC_OUT_OF_ORDER_EXEC_MODE_ENABLE, otherwise 1 manager)
            create_context_impl(pOutContext, contextFlags, 0);
        }
        CATCH_POSSIBLE_EXCEPTIONS(per_thread_api_log_str);
    }

    if (return_value != McResult::MC_NO_ERROR) {
        std::fprintf(stderr, "%s(...) -> %s\n", __FUNCTION__, per_thread_api_log_str.c_str());
    }

    return return_value;
}

MCAPI_ATTR McResult MCAPI_CALL mcCreateContextWithHelpers(McContext* pOutContext, McFlags contextFlags, uint32_t helperThreadCount)
{
    McResult return_value = McResult::MC_NO_ERROR;
    per_thread_api_log_str.clear();

    if (pOutContext == nullptr) {
        per_thread_api_log_str = "context ptr undef (NULL)";
        return_value = McResult::MC_INVALID_VALUE;
    } else {
        try {
            create_context_impl(pOutContext, contextFlags, helperThreadCount);
        }
        CATCH_POSSIBLE_EXCEPTIONS(per_thread_api_log_str);
    }

    if (return_value != McResult::MC_NO_ERROR) {
        std::fprintf(stderr, "%s(...) -> %s\n", __FUNCTION__, per_thread_api_log_str.c_str());
    }

    return return_value;
}

MCAPI_ATTR McResult MCAPI_CALL mcDebugMessageCallback(McContext pContext, pfn_mcDebugOutput_CALLBACK cb, const McVoid* userParam)
{
    McResult return_value = McResult::MC_NO_ERROR;
    per_thread_api_log_str.clear();

    if (pContext == nullptr) {
        per_thread_api_log_str = "context ptr (param0) undef (NULL)";
    } else if (cb == nullptr) {
        per_thread_api_log_str = "callback function ptr (param1) undef (NULL)";
    } else {
        try {
            debug_message_callback_impl(pContext, cb, userParam);
        }
        CATCH_POSSIBLE_EXCEPTIONS(per_thread_api_log_str);
    }

    if (!per_thread_api_log_str.empty()) {
        std::fprintf(stderr, "%s(...) -> %s\n", __FUNCTION__, per_thread_api_log_str.c_str());

        if (return_value == McResult::MC_NO_ERROR) // i.e. problem with basic local parameter checks
        {
            return_value = McResult::MC_INVALID_VALUE;
        }
    }

    return return_value;
}

MCAPI_ATTR McResult MCAPI_CALL mcGetDebugMessageLog(
    McContext context,
    McUint32 count, McSize bufSize,
    McDebugSource* sources, McDebugType* types, McDebugSeverity* severities,
    McSize* lengths, McChar* messageLog, McUint32* numFetched)
{
    McResult return_value = McResult::MC_NO_ERROR;
    per_thread_api_log_str.clear();

    if (context == nullptr) {
        per_thread_api_log_str = "context ptr (param0) undef (NULL)";
    } else if (
        count == 0) {
        per_thread_api_log_str = "count must be > 0";
    } else if (bufSize == 0) {
        per_thread_api_log_str = "bufSize must be > 0";
    } else if (numFetched == nullptr) {
        per_thread_api_log_str = "numFetched  undef (NULL)";
    } else {
        try {
            get_debug_message_log_impl(context,
                count, bufSize,
                sources, types, severities,
                lengths, messageLog, *numFetched);
        }
        CATCH_POSSIBLE_EXCEPTIONS(per_thread_api_log_str);
    }

    if (!per_thread_api_log_str.empty()) {
        std::fprintf(stderr, "%s(...) -> %s\n", __FUNCTION__, per_thread_api_log_str.c_str());
        if (return_value == McResult::MC_NO_ERROR) // i.e. problem with basic local parameter checks
        {
            return_value = McResult::MC_INVALID_VALUE;
        }
    }

    return return_value;
}

MCAPI_ATTR McResult MCAPI_CALL mcDebugMessageControl(McContext pContext, McDebugSource source, McDebugType type, McDebugSeverity severity, bool enabled)
{
    McResult return_value = McResult::MC_NO_ERROR;
    per_thread_api_log_str.clear();

    if (pContext == nullptr) {
        per_thread_api_log_str = "context ptr (param0) undef (NULL)";
    } else if (
        false == (source == McDebugSource::MC_DEBUG_SOURCE_ALL || //
            source == McDebugSource::MC_DEBUG_SOURCE_KERNEL || //
            source == McDebugSource::MC_DEBUG_SOURCE_ALL)) {
        per_thread_api_log_str = "debug source val (param1) invalid";
    } else if (
        false == (type == McDebugType::MC_DEBUG_TYPE_ALL || //
            type == McDebugType::MC_DEBUG_TYPE_DEPRECATED_BEHAVIOR || //
            type == McDebugType::MC_DEBUG_TYPE_ERROR || //
            type == McDebugType::MC_DEBUG_TYPE_OTHER)) {
        per_thread_api_log_str = "debug type val (param2) invalid";
    } else if (
        false == (severity == McDebugSeverity::MC_DEBUG_SEVERITY_HIGH || //
            severity == McDebugSeverity::MC_DEBUG_SEVERITY_MEDIUM || //
            severity == McDebugSeverity::MC_DEBUG_SEVERITY_LOW || //
            severity == McDebugSeverity::MC_DEBUG_SEVERITY_NOTIFICATION || //
            severity == McDebugSeverity::MC_DEBUG_SEVERITY_ALL)) {
        per_thread_api_log_str = "debug severity val (param3) invalid";
    } else {
        try {
            debug_message_control_impl(pContext, source, type, severity, enabled);
        }
        CATCH_POSSIBLE_EXCEPTIONS(per_thread_api_log_str);
    }

    if (!per_thread_api_log_str.empty()) {
        std::fprintf(stderr, "%s(...) -> %s\n", __FUNCTION__, per_thread_api_log_str.c_str());
        if (return_value == McResult::MC_NO_ERROR) // i.e. problem with basic local parameter checks
        {
            return_value = McResult::MC_INVALID_VALUE;
        }
    }

    return return_value;
}

MCAPI_ATTR McResult MCAPI_CALL mcGetInfo(const McContext context, McFlags info, McSize bytes, McVoid* pMem, McSize* pNumBytes)
{
    McResult return_value = McResult::MC_NO_ERROR;
    per_thread_api_log_str.clear();

    if (context == nullptr) {
        per_thread_api_log_str = "context ptr (param0) undef (NULL)";
    } else if (bytes != 0 && pMem == nullptr) {
        per_thread_api_log_str = "invalid specification (param2 & param3)";
    } else if (false == //
        (info == MC_CONTEXT_FLAGS || //
            info == MC_CONTEXT_MAX_DEBUG_MESSAGE_LENGTH || //
            info == MC_CONTEXT_GENERAL_POSITION_ENFORCEMENT_CONSTANT || //
            info == MC_CONTEXT_GENERAL_POSITION_ENFORCEMENT_ATTEMPTS)) // check all possible values
    {
        per_thread_api_log_str = "invalid info flag val (param1)";
    } else if ((info == MC_CONTEXT_FLAGS) && (pMem != nullptr && bytes != sizeof(McFlags))) {
        per_thread_api_log_str = "invalid byte size (param2)"; // leads to e.g. "out of bounds" memory access during memcpy
    } else {
        try {
            get_info_impl(context, info, bytes, pMem, pNumBytes);
        }
        CATCH_POSSIBLE_EXCEPTIONS(per_thread_api_log_str);
    }

    if (!per_thread_api_log_str.empty()) {
        std::fprintf(stderr, "%s(...) -> %s\n", __FUNCTION__, per_thread_api_log_str.c_str());
        if (return_value == McResult::MC_NO_ERROR) // i.e. problem with basic local parameter checks
        {
            return_value = McResult::MC_INVALID_VALUE;
        }
    }

    return return_value;
}

MCAPI_ATTR McResult MCAPI_CALL mcBindState(
    const McContext context,
    McFlags stateInfo,
    McSize bytes,
    const McVoid* pMem)
{
    McResult return_value = McResult::MC_NO_ERROR;
    per_thread_api_log_str.clear();

    if (context == nullptr) {
        per_thread_api_log_str = "context ptr (param0) undef (NULL)";
    } else if (bytes == 0) {
        per_thread_api_log_str = "invalid bytes ";
    } else if (pMem == nullptr) {
        per_thread_api_log_str = "invalid ptr (pMem)";
    } else if (false == //
        (stateInfo == MC_CONTEXT_GENERAL_POSITION_ENFORCEMENT_CONSTANT || //
            stateInfo == MC_CONTEXT_GENERAL_POSITION_ENFORCEMENT_ATTEMPTS ||//
            stateInfo == MC_CONTEXT_CONNECTED_COMPONENT_FACE_WINDING_ORDER)) // check all possible values
    {
        per_thread_api_log_str = "invalid stateInfo ";
    } else if (
        ((stateInfo == MC_CONTEXT_GENERAL_POSITION_ENFORCEMENT_CONSTANT) && bytes != sizeof(McDouble)) || //
        ((stateInfo == MC_CONTEXT_GENERAL_POSITION_ENFORCEMENT_ATTEMPTS) && bytes != sizeof(McUint32))|| //
        ((stateInfo == MC_CONTEXT_CONNECTED_COMPONENT_FACE_WINDING_ORDER) && bytes != sizeof(McConnectedComponentFaceWindingOrder))) {
        per_thread_api_log_str = "invalid num bytes"; // leads to e.g. "out of bounds" memory access during memcpy
    } else {
        try {
            bind_state_impl(context, stateInfo, bytes, pMem);
        }
        CATCH_POSSIBLE_EXCEPTIONS(per_thread_api_log_str);
    }

    if (!per_thread_api_log_str.empty()) {
        std::fprintf(stderr, "%s(...) -> %s\n", __FUNCTION__, per_thread_api_log_str.c_str());
        if (return_value == McResult::MC_NO_ERROR) // i.e. problem with basic local parameter checks
        {
            return_value = McResult::MC_INVALID_VALUE;
        }
    }

    return return_value;
}

MCAPI_ATTR McResult MCAPI_CALL mcCreateUserEvent(
    McEvent* event,
    McContext context)
{
    McResult return_value = McResult::MC_NO_ERROR;
    per_thread_api_log_str.clear();

    if (event == nullptr) {
        per_thread_api_log_str = "event ptr (param0) undef (NULL)";
    } else if (context == nullptr) {
        per_thread_api_log_str = "context handle undefined (NULL)";
    } else {
        try {
            create_user_event_impl(event, context);
        }
        CATCH_POSSIBLE_EXCEPTIONS(per_thread_api_log_str);
    }

    if (!per_thread_api_log_str.empty()) {
        std::fprintf(stderr, "%s(...) -> %s\n", __FUNCTION__, per_thread_api_log_str.c_str());
        if (return_value == McResult::MC_NO_ERROR) // i.e. problem with basic local parameter checks
        {
            return_value = McResult::MC_INVALID_VALUE;
        }
    }

    return return_value;
}

MCAPI_ATTR McResult MCAPI_CALL mcSetUserEventStatus(
    McEvent event,
    McInt32 execution_status)
{
    McResult return_value = McResult::MC_NO_ERROR;
    per_thread_api_log_str.clear();

    if (event == nullptr) {
        per_thread_api_log_str = "event ptr (param0) undef (NULL)";
    } else {
        try {
            set_user_event_status_impl(event, execution_status);
        }
        CATCH_POSSIBLE_EXCEPTIONS(per_thread_api_log_str);
    }

    if (!per_thread_api_log_str.empty()) {
        std::fprintf(stderr, "%s(...) -> %s\n", __FUNCTION__, per_thread_api_log_str.c_str());
        if (return_value == McResult::MC_NO_ERROR) // i.e. problem with basic local parameter checks
        {
            return_value = McResult::MC_INVALID_VALUE;
        }
    }

    return return_value;
}

MCAPI_ATTR McResult MCAPI_CALL mcGetEventInfo(const McEvent event, McFlags info, McSize bytes, McVoid* pMem, McSize* pNumBytes)
{
    McResult return_value = McResult::MC_NO_ERROR;
    per_thread_api_log_str.clear();

    if (event == nullptr) {
        per_thread_api_log_str = "context ptr (param0) undef (NULL)";
    } else if (bytes != 0 && pMem == nullptr) {
        per_thread_api_log_str = "invalid specification (param2 & param3)";
    } else if ((info == MC_EVENT_RUNTIME_EXECUTION_STATUS) && (pMem != nullptr && bytes != sizeof(McResult))) {
        per_thread_api_log_str = "invalid byte size (param2)"; // leads to e.g. "out of bounds" memory access during memcpy
    } else if ((info == MC_EVENT_COMMAND_EXECUTION_STATUS) && (pMem != nullptr && bytes != sizeof(McFlags))) {
        per_thread_api_log_str = "invalid byte size (param2)"; // leads to e.g. "out of bounds" memory access during memcpy
    } else if ((info == MC_EVENT_TIMESTAMP_SUBMIT || info == MC_EVENT_TIMESTAMP_START || info == MC_EVENT_TIMESTAMP_END) && (pMem != nullptr && bytes != sizeof(McSize))) {
        per_thread_api_log_str = "invalid byte size (param2)"; // leads to e.g. "out of bounds" memory access during memcpy
    } else {
        try {
            get_event_info_impl(event, info, bytes, pMem, pNumBytes);
        }
        CATCH_POSSIBLE_EXCEPTIONS(per_thread_api_log_str);
    }

    if (!per_thread_api_log_str.empty()) {
        std::fprintf(stderr, "%s(...) -> %s\n", __FUNCTION__, per_thread_api_log_str.c_str());
        if (return_value == McResult::MC_NO_ERROR) // i.e. problem with basic local parameter checks
        {
            return_value = McResult::MC_INVALID_VALUE;
        }
    }

    return return_value;
}

MCAPI_ATTR McResult MCAPI_CALL mcWaitForEvents(
    uint32_t numEventsInWaitlist,
    const McEvent* pEventWaitList)
{
    McResult return_value = McResult::MC_NO_ERROR;
    per_thread_api_log_str.clear();

    if (pEventWaitList == nullptr && numEventsInWaitlist > 0) {
        per_thread_api_log_str = "invalid event waitlist ptr (NULL)";
    } else if (pEventWaitList != nullptr && numEventsInWaitlist == 0) {
        per_thread_api_log_str = "invalid event waitlist size (zero)";
    } else {
        try {
            McResult waitliststatus = MC_NO_ERROR;
            wait_for_events_impl(numEventsInWaitlist, pEventWaitList, waitliststatus);

            if (waitliststatus != McResult::MC_NO_ERROR) {
                per_thread_api_log_str = "event in waitlist has an error";
            }
        }
        CATCH_POSSIBLE_EXCEPTIONS(per_thread_api_log_str);
    }

    if (!per_thread_api_log_str.empty()) {

        std::fprintf(stderr, "%s(...) -> %s\n", __FUNCTION__, per_thread_api_log_str.c_str());

        if (return_value == McResult::MC_NO_ERROR) // i.e. problem with basic local parameter checks
        {
            return_value = McResult::MC_INVALID_VALUE;
        }
    }

    return return_value;
}

MCAPI_ATTR McResult MCAPI_CALL mcSetEventCallback(
    McEvent eventHandle,
    pfn_McEvent_CALLBACK eventCallback,
    McVoid* data)
{
    McResult return_value = McResult::MC_NO_ERROR;
    per_thread_api_log_str.clear();

    if (eventHandle == nullptr) {
        per_thread_api_log_str = "invalid event ptr (NULL)";
    }
    if (eventCallback == nullptr) {
        per_thread_api_log_str = "invalid event callback function ptr (NULL)";
    } else {
        try {
            set_event_callback_impl(eventHandle, eventCallback, data);
        }
        CATCH_POSSIBLE_EXCEPTIONS(per_thread_api_log_str);
    }

    if (!per_thread_api_log_str.empty()) {

        std::fprintf(stderr, "%s(...) -> %s\n", __FUNCTION__, per_thread_api_log_str.c_str());

        if (return_value == McResult::MC_NO_ERROR) // i.e. problem with basic local parameter checks
        {
            return_value = McResult::MC_INVALID_VALUE;
        }
    }

    return return_value;
}

MCAPI_ATTR McResult MCAPI_CALL mcEnqueueDispatch(
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
    McEvent* pEvent)
{
   
    McResult return_value = McResult::MC_NO_ERROR;
    per_thread_api_log_str.clear();

    if (context == nullptr) {
        per_thread_api_log_str = "context ptr (param0) undef (NULL)";
    } else if (dispatchFlags == 0) {
        per_thread_api_log_str = "dispatch flags unspecified";
    } else if ((dispatchFlags & MC_DISPATCH_REQUIRE_THROUGH_CUTS) && //
        (dispatchFlags & MC_DISPATCH_FILTER_FRAGMENT_LOCATION_UNDEFINED)) {
        // The user states that she does not want a partial cut but yet also states that she
        // wants to keep fragments with partial cuts. These two options are mutually exclusive!
        per_thread_api_log_str = "use of mutually-exclusive flags: MC_DISPATCH_REQUIRE_THROUGH_CUTS & MC_DISPATCH_FILTER_FRAGMENT_LOCATION_UNDEFINED";
    } else if ((dispatchFlags & MC_DISPATCH_VERTEX_ARRAY_FLOAT) == 0 && (dispatchFlags & MC_DISPATCH_VERTEX_ARRAY_DOUBLE) == 0) {
        per_thread_api_log_str = "dispatch vertex aray type unspecified";
    } else if (pSrcMeshVertices == nullptr) {
        per_thread_api_log_str = "source-mesh vertex-position array ptr undef (NULL)";
    } else if (numSrcMeshVertices < 3) {
        per_thread_api_log_str = "invalid source-mesh vertex count";
    } else if (pSrcMeshFaceIndices == nullptr) {
        per_thread_api_log_str = "source-mesh face-index array ptr undef (NULL)";
    } /*else if (pSrcMeshFaceSizes == nullptr) {
        per_thread_api_log_str = "source-mesh face-size array ptr undef (NULL)";
    }*/
    else if (numSrcMeshFaces < 1) {
        per_thread_api_log_str = "invalid source-mesh vertex count";
    } else if (pCutMeshVertices == nullptr) {
        per_thread_api_log_str = "cut-mesh vertex-position array ptr undef (NULL)";
    } else if (numCutMeshVertices < 3) {
        per_thread_api_log_str = "invalid cut-mesh vertex count";
    } else if (pCutMeshFaceIndices == nullptr) {
        per_thread_api_log_str = "cut-mesh face-index array ptr undef (NULL)";
    } /*else if (pCutMeshFaceSizes == nullptr) {
        per_thread_api_log_str = "cut-mesh face-size array ptr undef (NULL)";
    } */
    else if (numCutMeshFaces < 1) {
        per_thread_api_log_str = "invalid cut-mesh vertex count";
    } else if (pEventWaitList == nullptr && numEventsInWaitlist > 0) {
        per_thread_api_log_str = "invalid event waitlist ptr (NULL)";
    } else if (pEventWaitList != nullptr && numEventsInWaitlist == 0) {
        per_thread_api_log_str = "invalid event waitlist size (zero)";
    } else if (pEventWaitList == nullptr && numEventsInWaitlist == 0 && pEvent == nullptr) {
        per_thread_api_log_str = "invalid event ptr (zero)";
    } else {
        try {
            dispatch_impl(
                context,
                dispatchFlags,
                pSrcMeshVertices,
                pSrcMeshFaceIndices,
                pSrcMeshFaceSizes,
                numSrcMeshVertices,
                numSrcMeshFaces,
                pCutMeshVertices,
                pCutMeshFaceIndices,
                pCutMeshFaceSizes,
                numCutMeshVertices,
                numCutMeshFaces,
                numEventsInWaitlist,
                pEventWaitList,
                pEvent);
        }
        CATCH_POSSIBLE_EXCEPTIONS(per_thread_api_log_str);
    }

    if (!per_thread_api_log_str.empty()) {

        std::fprintf(stderr, "%s(...) -> %s\n", __FUNCTION__, per_thread_api_log_str.c_str());

        if (return_value == McResult::MC_NO_ERROR) // i.e. problem with basic local parameter checks
        {
            return_value = McResult::MC_INVALID_VALUE;
        }
    }

    return return_value;
}

MCAPI_ATTR McResult MCAPI_CALL mcDispatch(
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
    uint32_t numCutMeshFaces)
{
    McEvent event = MC_NULL_HANDLE;

    McResult return_value = mcEnqueueDispatch(
        context,
        dispatchFlags,
        pSrcMeshVertices,
        pSrcMeshFaceIndices,
        pSrcMeshFaceSizes,
        numSrcMeshVertices,
        numSrcMeshFaces,
        pCutMeshVertices,
        pCutMeshFaceIndices,
        pCutMeshFaceSizes,
        numCutMeshVertices,
        numCutMeshFaces,
        0,
        nullptr,
        &event);

    if (return_value == MC_NO_ERROR) { // API parameter checks are fine
        if (event != MC_NULL_HANDLE) // event must exist to wait on and query
        {
            McResult waitliststatus = MC_NO_ERROR;

            wait_for_events_impl(1, &event, waitliststatus); // block until event of mcEnqueueDispatch is completed!

            if (waitliststatus != McResult::MC_NO_ERROR) {
                return_value = waitliststatus;
            }

            release_events_impl(1, &event); // destroy
        }
    }

    return return_value;
}

MCAPI_ATTR McResult MCAPI_CALL mcEnqueueDispatchPlanarSection(
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
    McEvent* pEvent)
    {
        McResult return_value = McResult::MC_NO_ERROR;
    per_thread_api_log_str.clear();

    if (context == nullptr) {
        per_thread_api_log_str = "context ptr (param0) undef (NULL)";
    } else if (dispatchFlags == 0) {
        per_thread_api_log_str = "dispatch flags unspecified";
    } else if ((dispatchFlags & MC_DISPATCH_REQUIRE_THROUGH_CUTS) && //
        (dispatchFlags & MC_DISPATCH_FILTER_FRAGMENT_LOCATION_UNDEFINED)) {
        // The user states that she does not want a partial cut but yet also states that she
        // wants to keep fragments with partial cuts. These two options are mutually exclusive!
        per_thread_api_log_str = "use of mutually-exclusive flags: MC_DISPATCH_REQUIRE_THROUGH_CUTS & MC_DISPATCH_FILTER_FRAGMENT_LOCATION_UNDEFINED";
    } else if ((dispatchFlags & MC_DISPATCH_VERTEX_ARRAY_FLOAT) == 0 && (dispatchFlags & MC_DISPATCH_VERTEX_ARRAY_DOUBLE) == 0) {
        per_thread_api_log_str = "dispatch vertex aray type unspecified";
    } else if (pSrcMeshVertices == nullptr) {
        per_thread_api_log_str = "source-mesh vertex-position array ptr undef (NULL)";
    } else if (numSrcMeshVertices < 3) {
        per_thread_api_log_str = "invalid source-mesh vertex count";
    } else if (pSrcMeshFaceIndices == nullptr) {
        per_thread_api_log_str = "source-mesh face-index array ptr undef (NULL)";
    } /*else if (pSrcMeshFaceSizes == nullptr) {
        per_thread_api_log_str = "source-mesh face-size array ptr undef (NULL)";
    }*/
    else if (numSrcMeshFaces < 1) {
        per_thread_api_log_str = "invalid source-mesh vertex count";
    } else if (pNormalVector == nullptr) {
        per_thread_api_log_str = "normal vector ptr undef (NULL)";
    } else if (pNormalVector[0] == 0.0 && pNormalVector[1] == 0.0 && pNormalVector[2] == 0.0) {
        per_thread_api_log_str = "invalid normal vector (zero vector)";
    }else if (sectionOffset <= 0 && sectionOffset >= 1.0) {
        per_thread_api_log_str = "invalid section offset parameter";
    } else if (pEventWaitList == nullptr && numEventsInWaitlist > 0) {
        per_thread_api_log_str = "invalid event waitlist ptr (NULL)";
    } else if (pEventWaitList != nullptr && numEventsInWaitlist == 0) {
        per_thread_api_log_str = "invalid event waitlist size (zero)";
    } else if (pEventWaitList == nullptr && numEventsInWaitlist == 0 && pEvent == nullptr) {
        per_thread_api_log_str = "invalid event ptr (zero)";
    } else {
        try {
            dispatch_planar_section_impl(
                context,
                dispatchFlags,
                pSrcMeshVertices,
                pSrcMeshFaceIndices,
                pSrcMeshFaceSizes,
                numSrcMeshVertices,
                numSrcMeshFaces,
                pNormalVector,
                sectionOffset,
                numEventsInWaitlist,
                pEventWaitList,
                pEvent);
        }
        CATCH_POSSIBLE_EXCEPTIONS(per_thread_api_log_str);
    }

    if (!per_thread_api_log_str.empty()) {

        std::fprintf(stderr, "%s(...) -> %s\n", __FUNCTION__, per_thread_api_log_str.c_str());

        if (return_value == McResult::MC_NO_ERROR) // i.e. problem with basic local parameter checks
        {
            return_value = McResult::MC_INVALID_VALUE;
        }
    }

    return return_value;
    }

MCAPI_ATTR McResult MCAPI_CALL mcEnqueueGetConnectedComponents(
    const McContext context,
    const McConnectedComponentType connectedComponentType,
    const uint32_t numEntries,
    McConnectedComponent* pConnComps,
    uint32_t* numConnComps,
    uint32_t numEventsInWaitlist,
    const McEvent* pEventWaitList,
    McEvent* pEvent)
{
    McResult return_value = McResult::MC_NO_ERROR;
    per_thread_api_log_str.clear();

    if (context == nullptr) {
        per_thread_api_log_str = "context ptr (param0) undef (NULL)";
    } else if (connectedComponentType == 0) {
        per_thread_api_log_str = "invalid type-parameter (param1) (0)";
    } else if (numConnComps == nullptr && pConnComps == nullptr) {
        per_thread_api_log_str = "output parameters undef (param3 & param4)";
    } else if (pEventWaitList == nullptr && numEventsInWaitlist > 0) {
        per_thread_api_log_str = "invalid event waitlist ptr (NULL)";
    } else if (pEventWaitList != nullptr && numEventsInWaitlist == 0) {
        per_thread_api_log_str = "invalid event waitlist size (zero)";
    } else if (pEventWaitList == nullptr && numEventsInWaitlist == 0 && pEvent == nullptr) {
        per_thread_api_log_str = "invalid event ptr (zero)";
    } else {
        try {

            get_connected_components_impl(context, connectedComponentType, numEntries, pConnComps, numConnComps, numEventsInWaitlist, pEventWaitList, pEvent);
        }
        CATCH_POSSIBLE_EXCEPTIONS(per_thread_api_log_str);
    }

    if (!per_thread_api_log_str.empty()) {

        std::fprintf(stderr, "%s(...) -> %s\n", __FUNCTION__, per_thread_api_log_str.c_str());

        if (return_value == McResult::MC_NO_ERROR) // i.e. problem with basic local parameter checks
        {
            return_value = McResult::MC_INVALID_VALUE;
        }
    }

    return return_value;
}

MCAPI_ATTR McResult MCAPI_CALL mcGetConnectedComponents(
    const McContext context,
    const McConnectedComponentType connectedComponentType,
    const uint32_t numEntries,
    McConnectedComponent* pConnComps,
    uint32_t* numConnComps)
{
    McEvent event = MC_NULL_HANDLE;
    McResult return_value = mcEnqueueGetConnectedComponents(context, connectedComponentType, numEntries, pConnComps, numConnComps, 0, nullptr, &event);
    if (event != MC_NULL_HANDLE) // event must exist to wait on and query
    {
        McResult waitliststatus = MC_NO_ERROR;

        wait_for_events_impl(1, &event, waitliststatus); // block until event of mcEnqueueDispatch is completed!

        if (waitliststatus != McResult::MC_NO_ERROR) {
            return_value = waitliststatus;
        }
        release_events_impl(1, &event); // destroy
    }
    return return_value;
}

MCAPI_ATTR McResult MCAPI_CALL mcEnqueueGetConnectedComponentData(
    const McContext context,
    const McConnectedComponent connCompId,
    McFlags queryFlags,
    McSize bytes,
    McVoid* pMem,
    McSize* pNumBytes,
    uint32_t numEventsInWaitlist,
    const McEvent* pEventWaitList,
    McEvent* pEvent)
{
    McResult return_value = McResult::MC_NO_ERROR;
    per_thread_api_log_str.clear();

    if (context == nullptr) {
        per_thread_api_log_str = "context ptr (param0) undef (NULL)";
    }
    if (connCompId == nullptr) {
        per_thread_api_log_str = "connected component ptr (param1) undef (NULL)";
    } else if (queryFlags == 0) {
        per_thread_api_log_str = "flags (param1) undef (0)";
    } else if (bytes != 0 && pMem == nullptr) {
        per_thread_api_log_str = "null parameter (param3 & param4)";
    } else if (pEventWaitList == nullptr && numEventsInWaitlist > 0) {
        per_thread_api_log_str = "invalid event waitlist ptr (NULL)";
    } else if (pEventWaitList != nullptr && numEventsInWaitlist == 0) {
        per_thread_api_log_str = "invalid event waitlist size (zero)";
    } else if (pEventWaitList == nullptr && numEventsInWaitlist == 0 && pEvent == nullptr) {
        per_thread_api_log_str = "invalid event ptr (zero)";
    } else {
        try {
            get_connected_component_data_impl(context, connCompId, queryFlags, bytes, pMem, pNumBytes, numEventsInWaitlist, pEventWaitList, pEvent);
        }
        CATCH_POSSIBLE_EXCEPTIONS(per_thread_api_log_str);
    }

    if (!per_thread_api_log_str.empty()) {

        std::fprintf(stderr, "%s(...) -> %s\n", __FUNCTION__, per_thread_api_log_str.c_str());

        if (return_value == McResult::MC_NO_ERROR) // i.e. problem with basic local parameter checks
        {
            return_value = McResult::MC_INVALID_VALUE;
        }
    }

    return return_value;
}

MCAPI_ATTR McResult MCAPI_CALL mcGetConnectedComponentData(
    const McContext context,
    const McConnectedComponent connCompId,
    McFlags queryFlags,
    McSize bytes,
    McVoid* pMem,
    McSize* pNumBytes)
{
    McEvent event = MC_NULL_HANDLE;
    McResult return_value = mcEnqueueGetConnectedComponentData(context, connCompId, queryFlags, bytes, pMem, pNumBytes, 0, nullptr, &event);
    if (event != MC_NULL_HANDLE) // event must exist to wait on and query
    {
        McResult waitliststatus = MC_NO_ERROR;

        wait_for_events_impl(1, &event, waitliststatus); // block until event of mcEnqueueDispatch is completed!

        if (waitliststatus != McResult::MC_NO_ERROR) {
            return_value = waitliststatus;
        }

        release_events_impl(1, &event); // destroy
    }
    return return_value;
}

MCAPI_ATTR McResult MCAPI_CALL mcReleaseEvents(
    uint32_t numEvents,
    const McEvent* pEvents)
{
    McResult return_value = McResult::MC_NO_ERROR;
    per_thread_api_log_str.clear();

    if (numEvents > 0 && pEvents == NULL) {
        per_thread_api_log_str = "invalid pointer to events";
    } else if (numEvents == 0 && pEvents != NULL) {
        per_thread_api_log_str = "number of events not set";
    } else {
        try {
            release_events_impl(numEvents, pEvents);
        }
        CATCH_POSSIBLE_EXCEPTIONS(per_thread_api_log_str);
    }

    if (!per_thread_api_log_str.empty()) {

        std::fprintf(stderr, "%s(...) -> %s\n", __FUNCTION__, per_thread_api_log_str.c_str());

        if (return_value == McResult::MC_NO_ERROR) // i.e. problem with basic local parameter checks
        {
            return_value = McResult::MC_INVALID_VALUE;
        }
    }

    return return_value;
}

MCAPI_ATTR McResult MCAPI_CALL mcReleaseConnectedComponents(
    const McContext context,
    uint32_t numConnComps,
    const McConnectedComponent* pConnComps)
{
    McResult return_value = McResult::MC_NO_ERROR;
    per_thread_api_log_str.clear();

    if (context == nullptr) {
        per_thread_api_log_str = "context ptr (param0) undef (NULL)";
    } else if (numConnComps > 0 && pConnComps == NULL) {
        per_thread_api_log_str = "invalid pointer to connected components";
    } else if (numConnComps == 0 && pConnComps != NULL) {
        per_thread_api_log_str = "number of connected components not set";
    } else {
        try {
            release_connected_components_impl(context, numConnComps, pConnComps);
        }
        CATCH_POSSIBLE_EXCEPTIONS(per_thread_api_log_str);
    }

    if (!per_thread_api_log_str.empty()) {

        std::fprintf(stderr, "%s(...) -> %s\n", __FUNCTION__, per_thread_api_log_str.c_str());

        if (return_value == McResult::MC_NO_ERROR) // i.e. problem with basic local parameter checks
        {
            return_value = McResult::MC_INVALID_VALUE;
        }
    }

    return return_value;
}

MCAPI_ATTR McResult MCAPI_CALL mcReleaseContext(const McContext context)
{
    McResult return_value = McResult::MC_NO_ERROR;
    per_thread_api_log_str.clear();

    if (context == nullptr) {
        per_thread_api_log_str = "context ptr (param0) undef (NULL)";
    } else {
        try {
            release_context_impl(context);
        }
        CATCH_POSSIBLE_EXCEPTIONS(per_thread_api_log_str);
    }

    if (!per_thread_api_log_str.empty()) {

        std::fprintf(stderr, "%s(...) -> %s\n", __FUNCTION__, per_thread_api_log_str.c_str());

        if (return_value == McResult::MC_NO_ERROR) // i.e. problem with basic local parameter checks
        {
            return_value = McResult::MC_INVALID_VALUE;
        }
    }

    return return_value;
}
