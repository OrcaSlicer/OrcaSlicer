#include "mcut/internal/frontend.h"
#include "mcut/internal/preproc.h"

#include "mcut/internal/hmesh.h"
#include "mcut/internal/math.h"
#include "mcut/internal/utils.h"

#include <algorithm>
#include <array>
#include <fstream>

#include <memory>

#include <numeric> // iota
#include <stdio.h>
#include <string.h>
#include <unordered_map>

#include "mcut/internal/cdt/cdt.h"
#include "mcut/internal/timer.h"

#if defined(PROFILING_BUILD)
thread_local std::stack<std::unique_ptr<mini_timer>> g_thrd_loc_timerstack;
#endif

thread_local std::string per_thread_api_log_str;

threadsafe_list<std::shared_ptr<context_t>> g_contexts = {};
threadsafe_list<std::shared_ptr<event_t>> g_events = {};
std::atomic<std::uintptr_t> g_objects_counter; // a counter that is used to assign a unique value to e.g. a McContext handle that will be returned to the user
std::once_flag g_objects_counter_init_flag; // flag used to initialise "g_objects_counter" with "std::call_once"

void create_context_impl(McContext* pOutContext, McFlags flags, uint32_t helperThreadCount)
{
#if !defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
    UNUSED(helperThreadCount);
#endif

    MCUT_ASSERT(pOutContext != nullptr);

    std::call_once(g_objects_counter_init_flag, []() { g_objects_counter.store(0xDECAF); /*any non-ero value*/ });

    const McContext handle = reinterpret_cast<McContext>(g_objects_counter.fetch_add(1, std::memory_order_relaxed));
    g_contexts.push_front(std::shared_ptr<context_t>(
        new context_t(handle, flags
#if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
            ,
            helperThreadCount
#endif
            )));

    *pOutContext = handle;
}

void debug_message_callback_impl(
    McContext contextHandle,
    pfn_mcDebugOutput_CALLBACK cb,
    const McVoid* userParam)
{
    MCUT_ASSERT(contextHandle != nullptr);
    MCUT_ASSERT(cb != nullptr);

    std::shared_ptr<context_t> context_ptr = g_contexts.find_first_if([=](const std::shared_ptr<context_t> cptr) { return cptr->m_user_handle == contextHandle; });

    // std::map<McContext, std::unique_ptr<context_t>>::iterator context_entry_iter = g_contexts.find(contextHandle);

    if (context_ptr == nullptr) {
        // "contextHandle" may not be NULL but that does not mean it maps to
        // a valid object in "g_contexts"
        throw std::invalid_argument("invalid context");
    }
    // const std::unique_ptr<context_t>& context_uptr = context_entry_iter->second;

    // set callback function ptr, and user pointer
    context_ptr->set_debug_callback_data(cb, userParam);
}

void get_debug_message_log_impl(McContext context,
    McUint32 count, McSize bufSize,
    McDebugSource* sources, McDebugType* types, McDebugSeverity* severities,
    McSize* lengths, McChar* messageLog, McUint32& numFetched)
{
    MCUT_ASSERT(context != nullptr);

    std::shared_ptr<context_t> context_ptr = g_contexts.find_first_if([=](const std::shared_ptr<context_t> cptr) { return cptr->m_user_handle == context; });

    if (context_ptr == nullptr) {
        throw std::invalid_argument("invalid context");
    }

    numFetched = 0;

    if (messageLog == nullptr) {
        context_ptr->dbg_cb(MC_DEBUG_SOURCE_API, MC_DEBUG_TYPE_OTHER, 0, MC_DEBUG_SEVERITY_NOTIFICATION, "output messageLog is NULL. Return.");
        return;
    }

    McSize messageLogOffset = 0;

    const uint32_t N = std::min((uint32_t)count, (uint32_t)context_ptr->m_debug_logs.size());

    // for internal message
    for (numFetched = 0; numFetched < N; ++numFetched) {

        const context_t::debug_log_msg_t& cur_dbg_msg = context_ptr->m_debug_logs[numFetched];
        const McSize msg_length = (McSize)cur_dbg_msg.str.size();
        const McSize newLengthOfActualMessageLog = (messageLogOffset + msg_length);

        if (newLengthOfActualMessageLog > bufSize) {
            break; // stop
        }

        if (sources != nullptr) {
            sources[numFetched] = cur_dbg_msg.source;
        }

        if (types != nullptr) {
            types[numFetched] = cur_dbg_msg.type;
        }

        if (severities != nullptr) {
            severities[numFetched] = cur_dbg_msg.severity;
        }

        if (lengths != nullptr) {
            lengths[numFetched] = msg_length;
        }

        // copy into output array
        memcpy(messageLog + messageLogOffset, cur_dbg_msg.str.data(), msg_length);

        messageLogOffset = newLengthOfActualMessageLog;
    }
}

// find the number of trailing zeros in v
// http://graphics.stanford.edu/~seander/bithacks.html#ZerosOnRightLinear
inline int trailing_zeroes(uint32_t v)
{
    int r; // the result goes here
#ifdef _WIN32
#pragma warning(disable : 4146) // "unary minus operator applied to unsigned type, result still unsigned"
#endif // #ifdef _WIN32
    float f = (float)(v & -v); // cast the least significant bit in v to a float
#ifdef _WIN32
#pragma warning(default : 4146)
#endif // #ifdef _WIN32

// dereferencing type-punned pointer will break strict-aliasing rules
#if __linux__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
#endif

    r = (*(uint32_t*)&f >> 23) - 0x7f;

#if __linux__
#pragma GCC diagnostic pop
#endif
    return r;
}

// https://stackoverflow.com/questions/47981/how-do-you-set-clear-and-toggle-a-single-bit
inline int set_bit(uint32_t v, uint32_t pos)
{
    v |= 1U << pos;
    return v;
}

inline int clear_bit(uint32_t v, uint32_t pos)
{
    v &= ~(1UL << pos);
    return v;
}

void debug_message_control_impl(
    McContext contextHandle,
    McDebugSource sourceBitfieldParam,
    McDebugType typeBitfieldParam,
    McDebugSeverity severityBitfieldParam,
    bool enabled)
{
    std::shared_ptr<context_t> context_ptr = g_contexts.find_first_if([=](const std::shared_ptr<context_t> cptr) { return cptr->m_user_handle == contextHandle; });

    if (context_ptr == nullptr) {
        throw std::invalid_argument("invalid context");
    }

    //
    // Debug "source" flag
    //

    // for each possible "source" flag
    for (auto i : { MC_DEBUG_SOURCE_API, MC_DEBUG_SOURCE_KERNEL }) {
        if (sourceBitfieldParam & i) { // was it set/included by the user (to be enabled/disabled)?
            int n = trailing_zeroes(MC_DEBUG_SOURCE_ALL & i); // get coords of bit representing current "source" flag
            if (enabled) { // does the user want to enabled this information (from being logged in the debug callback function)
                context_ptr->dbgCallbackBitfieldSource = set_bit(context_ptr->dbgCallbackBitfieldSource, n);
            } else { // ... user wants to disable this information
                context_ptr->dbgCallbackBitfieldSource = clear_bit(context_ptr->dbgCallbackBitfieldSource, n);
            }
        }
    }

    //
    // Debug "type" flag
    //

    for (auto i : { MC_DEBUG_TYPE_DEPRECATED_BEHAVIOR, MC_DEBUG_TYPE_ERROR, MC_DEBUG_TYPE_OTHER }) {
        if (typeBitfieldParam & i) {

            const int n = trailing_zeroes(MC_DEBUG_TYPE_ALL & i);

            if (enabled) {
                context_ptr->dbgCallbackBitfieldType = set_bit(context_ptr->dbgCallbackBitfieldType, n);
            } else {

                context_ptr->dbgCallbackBitfieldType = clear_bit(context_ptr->dbgCallbackBitfieldType, n);
            }
        }
    }

    //
    // Debug "severity" flag
    //

    for (auto i : { MC_DEBUG_SEVERITY_HIGH, MC_DEBUG_SEVERITY_LOW, MC_DEBUG_SEVERITY_MEDIUM, MC_DEBUG_SEVERITY_NOTIFICATION }) {
        if (severityBitfieldParam & i) {

            const int n = trailing_zeroes(MC_DEBUG_SEVERITY_ALL & i);

            if (enabled) {
                context_ptr->dbgCallbackBitfieldSeverity = set_bit(context_ptr->dbgCallbackBitfieldSeverity, n);
            } else {
                context_ptr->dbgCallbackBitfieldSeverity = clear_bit(context_ptr->dbgCallbackBitfieldSeverity, n);
            }
        }
    }
}

void get_info_impl(
    const McContext contextHandle,
    McFlags info,
    McSize bytes,
    McVoid* pMem,
    McSize* pNumBytes)
{
    std::shared_ptr<context_t> context_ptr = g_contexts.find_first_if([=](const std::shared_ptr<context_t> cptr) { return cptr->m_user_handle == contextHandle; });

    if (context_ptr == nullptr) {
        throw std::invalid_argument("invalid context");
    }

    switch (info) {
    case MC_CONTEXT_FLAGS: {
        McFlags flags = context_ptr->get_flags();
        if (pMem == nullptr) {
            *pNumBytes = sizeof(McFlags);
        } else {
            if (bytes < sizeof(McFlags)) {
                throw std::invalid_argument("invalid bytes");
            }
            memcpy(pMem, reinterpret_cast<McVoid*>(&flags), bytes);
        }
        break;
    }
    case MC_CONTEXT_MAX_DEBUG_MESSAGE_LENGTH: {
        if (pMem == nullptr) {
            *pNumBytes = sizeof(McSize);
        } else {
            std::lock_guard<std::mutex> lock(context_ptr->debugCallbackMutex);
            McSize sizeMax = 0;
            for (McUint32 i = 0; i < (McUint32)context_ptr->m_debug_logs.size(); ++i) {
                sizeMax = std::max((McSize)sizeMax, (McSize)context_ptr->m_debug_logs[i].str.size());
            }
            memcpy(pMem, reinterpret_cast<McVoid*>(&sizeMax), sizeof(McSize));
        }

    } break;
    case MC_CONTEXT_GENERAL_POSITION_ENFORCEMENT_CONSTANT: {
        if (pMem == nullptr) {
            *pNumBytes = sizeof(McDouble);
        } else {
            const McDouble gpec = context_ptr->get_general_position_enforcement_constant();
            memcpy(pMem, reinterpret_cast<const McDouble*>(&gpec), sizeof(McDouble));
        }
    } break;

    case MC_CONTEXT_GENERAL_POSITION_ENFORCEMENT_ATTEMPTS: {
        if (pMem == nullptr) {
            *pNumBytes = sizeof(McUint32);
        } else {
            const McUint32 attempts = context_ptr->get_general_position_enforcement_attempts();
            memcpy(pMem, reinterpret_cast<const McDouble*>(&attempts), sizeof(McUint32));
        }
    } break;
    case MC_CONTEXT_CONNECTED_COMPONENT_FACE_WINDING_ORDER: {
        if (pMem == nullptr) {
            *pNumBytes = sizeof(McConnectedComponentFaceWindingOrder);
        } else {
            const McConnectedComponentFaceWindingOrder wo = context_ptr->get_connected_component_winding_order();
            memcpy(pMem, reinterpret_cast<const McConnectedComponentFaceWindingOrder*>(&wo), sizeof(McConnectedComponentFaceWindingOrder));
        }
    } break;

    default:
        throw std::invalid_argument("unknown info parameter");
        break;
    }
}

void bind_state_impl(
    const McContext context,
    McFlags stateInfo,
    McSize bytes,
    const McVoid* pMem)
{
    std::shared_ptr<context_t> context_ptr = g_contexts.find_first_if([=](const std::shared_ptr<context_t> cptr) { return cptr->m_user_handle == context; });

    if (context_ptr == nullptr) {
        throw std::invalid_argument("invalid context");
    }

    switch (stateInfo) {
    case MC_CONTEXT_GENERAL_POSITION_ENFORCEMENT_CONSTANT: {
        McDouble value;
        memcpy(&value, pMem, bytes);
        context_ptr->dbg_cb(MC_DEBUG_SOURCE_API, MC_DEBUG_TYPE_OTHER, 0, MC_DEBUG_SEVERITY_NOTIFICATION, "general coords enforcement constant set to " + std::to_string(value));
        if (value <= 0) {
            throw std::invalid_argument("invalid general coords enforcement constant");
        }
        context_ptr->set_general_position_enforcement_constant(value);

    } break;
    case MC_CONTEXT_GENERAL_POSITION_ENFORCEMENT_ATTEMPTS: {
        McUint32 value;
        memcpy(&value, pMem, bytes);
        context_ptr->dbg_cb(MC_DEBUG_SOURCE_API, MC_DEBUG_TYPE_OTHER, 0, MC_DEBUG_SEVERITY_NOTIFICATION, "general position enforcement attempts set to " + std::to_string(value));
        if (value < 1) {
            throw std::invalid_argument("invalid general coords enforcement attempts -> " + std::to_string(value));
        }
        context_ptr->set_general_position_enforcement_attempts(value);

    } break;
    case MC_CONTEXT_CONNECTED_COMPONENT_FACE_WINDING_ORDER: {
        McConnectedComponentFaceWindingOrder value;
        memcpy(&value, pMem, bytes);
        context_ptr->dbg_cb(MC_DEBUG_SOURCE_API, MC_DEBUG_TYPE_OTHER, 0, MC_DEBUG_SEVERITY_NOTIFICATION, "winding-order set to " + std::to_string(value));

        if (value != McConnectedComponentFaceWindingOrder::MC_CONNECTED_COMPONENT_FACE_WINDING_ORDER_AS_GIVEN && //
            value != McConnectedComponentFaceWindingOrder::MC_CONNECTED_COMPONENT_FACE_WINDING_ORDER_REVERSED) {
            throw std::invalid_argument("invalid winding-order param value");
        }
        context_ptr->set_connected_component_winding_order(value);
    } break;
    default:
        throw std::invalid_argument("unknown info parameter");
        break;
    }
}

void create_user_event_impl(McEvent* eventHandle, McContext context)
{
    std::shared_ptr<context_t> context_ptr = g_contexts.find_first_if([=](const std::shared_ptr<context_t> cptr) { return cptr->m_user_handle == context; });

    if (context_ptr == nullptr) {
        throw std::invalid_argument("invalid context");
    }

    //
    // create the event object associated with the enqueued task
    //

    std::shared_ptr<event_t> user_event_ptr = std::shared_ptr<event_t>(new event_t(
        reinterpret_cast<McEvent>(g_objects_counter.fetch_add(1, std::memory_order_relaxed)),
        McCommandType::MC_COMMAND_USER));

    MCUT_ASSERT(user_event_ptr != nullptr);

    g_events.push_front(user_event_ptr);

    // user_event_ptr->m_user_handle = reinterpret_cast<McEvent>(g_objects_counter.fetch_add(1, std::memory_order_relaxed));
    user_event_ptr->m_profiling_enabled = (context_ptr->get_flags() & MC_PROFILING_ENABLE) != 0;
    // user_event_ptr->m_command_type = McCommandType::MC_COMMAND_USER;
    user_event_ptr->m_context = context;
    user_event_ptr->m_responsible_thread_id = 0; // initialized but unused for user event

    user_event_ptr->log_submit_time();

    std::weak_ptr<event_t> user_event_weak_ptr(user_event_ptr);

    user_event_ptr->m_user_API_command_task_emulator = std::unique_ptr<std::packaged_task<void()>>(new std::packaged_task<void()>(
        // will be "called" when user updates the command status to MC_COMPLETE
        [user_event_weak_ptr]() {
            if (user_event_weak_ptr.expired()) {
                throw std::runtime_error("user event expired");
            }

            std::shared_ptr<event_t> event_ptr = user_event_weak_ptr.lock();

            if (event_ptr == nullptr) {
                throw std::runtime_error("user event null");
            }

            event_ptr->log_start_time();
        }));

    user_event_ptr->m_future = user_event_ptr->m_user_API_command_task_emulator->get_future(); // the future we can later wait on via mcWaitForEVents
    user_event_ptr->m_responsible_thread_id = MC_UNDEFINED_VALUE; // some user thread

    *eventHandle = user_event_ptr->m_user_handle;
}

/**
 * execution_status specifies the new execution status to be set and can be CL_​COMPLETE or a negative integer value to indicate an error.
 * A negative integer value causes all enqueued commands that wait on this user
 * event to be terminated.
 */
void set_user_event_status_impl(McEvent event, McInt32 execution_status)
{
    std::shared_ptr<event_t> event_ptr = g_events.find_first_if([=](const std::shared_ptr<event_t> ptr) { return ptr->m_user_handle == event; });

    if (event_ptr == nullptr) {
        throw std::invalid_argument("invalid event");
    }

    McResult userEventErrorCode = McResult::MC_NO_ERROR;

    std::unique_ptr<std::packaged_task<void()>>& associated_task_emulator = event_ptr->m_user_API_command_task_emulator;
    MCUT_ASSERT(associated_task_emulator != nullptr);
    // simply logs the start time and makes the std::packaged_task future object ready
    associated_task_emulator->operator()(); // call the internal "task" function to update event status

    switch (execution_status) {
    case McEventCommandExecStatus::MC_COMPLETE: {
        event_ptr->log_end_time();
    } break;
    default: {
        MCUT_ASSERT(execution_status < 0); // an error
        switch (execution_status) {
        case McResult::MC_INVALID_OPERATION:
        case McResult::MC_INVALID_VALUE:
        case McResult::MC_OUT_OF_MEMORY: {
            userEventErrorCode = (McResult)execution_status;
        } break;
        default: {
            throw std::invalid_argument("invalid command status");
        }
        }
    }
    }

    // invoke call back
    event_ptr->notify_task_complete(userEventErrorCode); // updated event state to indicate task completion (lock-based)
}

void get_event_info_impl(
    const McEvent event,
    McFlags info,
    McSize bytes,
    McVoid* pMem,
    McSize* pNumBytes)
{
    std::shared_ptr<event_t> event_ptr = g_events.find_first_if([=](const std::shared_ptr<event_t> ptr) { return ptr->m_user_handle == event; });

    if (event_ptr == nullptr) {
        throw std::invalid_argument("invalid event");
    }

    switch (info) {
    case MC_EVENT_RUNTIME_EXECUTION_STATUS: {

        if (pMem == nullptr) {
            *pNumBytes = sizeof(McResult);
        } else {
            if (bytes < sizeof(McResult)) {
                throw std::invalid_argument("invalid bytes");
            }
            McResult status = (McResult)event_ptr->m_runtime_exec_status.load();
            memcpy(pMem, reinterpret_cast<McVoid*>(&status), bytes);
        }
        break;
    }
    case MC_EVENT_TIMESTAMP_SUBMIT:
    case MC_EVENT_TIMESTAMP_START:
    case MC_EVENT_TIMESTAMP_END: {
        if (pMem == nullptr) {
            *pNumBytes = sizeof(McSize);
        } else {
            if (bytes < sizeof(McSize)) {
                throw std::invalid_argument("invalid bytes");
            }
            McSize nanoseconds_since_epoch = 0;
            if (info == MC_EVENT_TIMESTAMP_SUBMIT) {
                nanoseconds_since_epoch = event_ptr->m_timestamp_submit.load();
            } else if (info == MC_EVENT_TIMESTAMP_START) {
                nanoseconds_since_epoch = event_ptr->m_timestamp_start.load();
            } else if (info == MC_EVENT_TIMESTAMP_END) {
                nanoseconds_since_epoch = event_ptr->m_timestamp_end.load();
            }
            MCUT_ASSERT(nanoseconds_since_epoch != 0);
            memcpy(pMem, reinterpret_cast<McVoid*>(&nanoseconds_since_epoch), bytes);
        }
        break;
    }
    case MC_EVENT_COMMAND_EXECUTION_STATUS: {
        if (pMem == nullptr) {
            *pNumBytes = sizeof(McEventCommandExecStatus);
        } else {
            if (bytes < sizeof(McEventCommandExecStatus)) {
                throw std::invalid_argument("invalid bytes");
            }
            McEventCommandExecStatus status = (McEventCommandExecStatus)event_ptr->m_command_exec_status.load();
            memcpy(pMem, reinterpret_cast<McVoid*>(&status), bytes);
        }
    } break;
    case MC_EVENT_COMMAND_TYPE: {
        if (pMem == nullptr) {
            *pNumBytes = sizeof(McCommandType);
        } else {
            if (bytes < sizeof(McCommandType)) {
                throw std::invalid_argument("invalid bytes");
            }
            McCommandType cmdType = (McCommandType)event_ptr->m_command_type;
            memcpy(pMem, reinterpret_cast<McVoid*>(&cmdType), bytes);
        }
    } break;
    case MC_EVENT_CONTEXT: {
        if (pMem == nullptr) {
            *pNumBytes = sizeof(McContext);
        } else {
            if (bytes < sizeof(McContext)) {
                throw std::invalid_argument("invalid bytes");
            }
            McContext ctxt = (McContext)event_ptr->m_context;
            memcpy(pMem, reinterpret_cast<McVoid*>(&ctxt), bytes);
        }
    } break;
    default:
        throw std::invalid_argument("unknown info parameter");
        break;
    }
}

void wait_for_events_impl(
    uint32_t numEventsInWaitlist,
    const McEvent* pEventWaitList, McResult& runtimeStatusFromAllPrecedingEvents)
{
    for (uint32_t i = 0; i < numEventsInWaitlist; ++i) {
        McEvent eventHandle = pEventWaitList[i];

        std::shared_ptr<event_t> event_ptr = g_events.find_first_if([=](const std::shared_ptr<event_t> eptr) { return eptr->m_user_handle == eventHandle; });

        if (event_ptr == nullptr) {
            // "contextHandle" may not be NULL but that does not mean it maps to
            // a valid object in "g_contexts"
            throw std::invalid_argument("null event object");
        } else {
            if (event_ptr->m_future.valid()) {

                event_ptr->m_future.wait(); // block until event task is finished

                runtimeStatusFromAllPrecedingEvents = (McResult)event_ptr->m_runtime_exec_status.load();
                if (runtimeStatusFromAllPrecedingEvents != McResult::MC_NO_ERROR) {
                    // indicate that a task waiting on any one of the event in pEventWaitList
                    // must not proceed because a runtime error occurred
                    break;
                }
            }
        }
    }
}

void set_event_callback_impl(
    McEvent eventHandle,
    pfn_McEvent_CALLBACK eventCallback,
    McVoid* data)
{
    std::shared_ptr<event_t> event_ptr = g_events.find_first_if([=](const std::shared_ptr<event_t> eptr) { return eptr->m_user_handle == eventHandle; });

    if (event_ptr == nullptr) {
        // "contextHandle" may not be NULL but that does not mean it maps to
        // a valid object in "g_contexts"
        throw std::invalid_argument("unknown event object");
    }

    event_ptr->set_callback_data(eventHandle, eventCallback, data);
}

void dispatch_impl(
    McContext contextHandle,
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
    std::shared_ptr<context_t> context_ptr = g_contexts.find_first_if([=](const std::shared_ptr<context_t> cptr) { return cptr->m_user_handle == contextHandle; });

    if (context_ptr == nullptr) {
        throw std::invalid_argument("invalid context");
    }

    std::weak_ptr<context_t> context_weak_ptr(context_ptr);

    // submit the dispatch call to be executed asynchronously and return the future
    // object that will be waited on as an event
    const McEvent event_handle = context_ptr->prepare_and_submit_API_task(
        MC_COMMAND_DISPATCH, numEventsInWaitlist, pEventWaitList,
        [=]() {
            if (!context_weak_ptr.expired()) {
                std::shared_ptr<context_t> context = context_weak_ptr.lock();
                if (context) {
                    preproc(
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
                        numCutMeshFaces);
                }
            }
        });

    MCUT_ASSERT(pEvent != nullptr);

    *pEvent = event_handle;
}

template <typename T>
T clamp(const T& n, const T& lower, const T& upper)
{
    return std::max(lower, std::min(n, upper));
}

void generate_supertriangle_from_mesh_vertices(
    std::vector<McChar>& supertriangle_vertices,
    std::vector<McIndex>& supertriangle_indices,
    McFlags dispatchFlags,
    const McVoid* pMeshVertices,
    uint32_t numMeshVertices,
    const McDouble* pNormalVector,
    const McDouble sectionOffset, const McDouble eps)
{
    // did the user give us an array of doubles? (otherwise floats)
    const bool have_double = (dispatchFlags & MC_DISPATCH_VERTEX_ARRAY_DOUBLE) != 0;
    // size (number of bytes) of input floating point type
    const std::size_t flt_size = (have_double ? sizeof(double) : sizeof(float));
    // normalized input normal vector
    const vec3 n = normalize(vec3(pNormalVector[0], pNormalVector[1], pNormalVector[2]));

    // minimum projection of mesh vertices along the normal vector
    double proj_min = 1e10;
    // maximum projection of mesh vertices along the normal vector
    double proj_max = -proj_min;

    // mesh bbox extents
    vec3 bbox_min(1e10);
    vec3 bbox_max(-1e10);

    vec3 mean(0.0);

    // mesh-vertex with the most-minimum projection onto the normal vector
    vec3 most_min_vertex_pos;
    // mesh-vertex with the most-maximum projection onto the normal vector
    vec3 most_max_vertex_pos;

    for (uint32_t i = 0; i < numMeshVertices; ++i) { // for each vertex

        // input (raw) pointer in bytes
        const McChar* vptr = ((McChar*)pMeshVertices) + (i * flt_size * 3);
        vec3 coords; // coordinates of current vertex

        for (uint32_t j = 0; j < 3; ++j) { // for each component

            double coord;
            const McChar* const srcptr = vptr + (j * flt_size);

            if (have_double) {
                memcpy(&coord, srcptr, flt_size);
            } else {
                float tmp;
                memcpy(&tmp, srcptr, flt_size);
                coord = tmp;
            }

            bbox_min[j] = std::min(bbox_min[j], coord);
            bbox_max[j] = std::max(bbox_max[j], coord);

            coords[j] = coord;
        }
        mean = mean+coords;
        const double dot = dot_product(n, coords);

        if (dot < proj_min) {
            most_min_vertex_pos = coords;
            proj_min = dot;
        }

        if (dot > proj_max) {
            most_max_vertex_pos = coords;
            proj_max = dot;
        }
    }
    mean = mean/numMeshVertices;
    // length of bounding box diagonal
    const double bbox_diag = length(bbox_max - bbox_min);
    // length from vertex with most-minimum projection to vertex with most-maximum projection
    const double max_span = length(most_max_vertex_pos - most_min_vertex_pos);
    // parameter indicating distance along the span from vertex with most-minimum projection to vertex with most-maximum projection
    const double alpha = clamp(sectionOffset, eps, 1.0 - eps);
    // actual distance along the span from vertex with most-minimum projection to vertex with most-maximum projection
    const double shiftby = (alpha * max_span);

    const vec3 centroid = most_min_vertex_pos + (n * shiftby);

    // absolute value of the largest component of the normal vector
    double max_normal_comp_val_abs = -1e10;
    // index of the largest component of the normal vector
    uint32_t max_normal_comp_idx = 0;

    for (uint32_t i = 0; i < 3; ++i) {

        const double comp = n[i];
        const double comp_abs = std::fabs(comp);

        if (comp_abs > max_normal_comp_val_abs) {
            max_normal_comp_idx = i;
            max_normal_comp_val_abs = comp_abs;
        }
    }

    vec3 w(0.0);
    w[max_normal_comp_idx] = 1.0;
    if (w == n) {
        w[max_normal_comp_idx] = 0.0;
        w[(max_normal_comp_idx + 1) % 3] = 1.0;
    }

    const vec3 u = cross_product(n, w);
    const vec3 v = cross_product(n, u);

    const double mean_dot_n = dot_product(mean, n);
    vec3 mean_on_plane = mean - n*mean_dot_n;

    vec3 uv_pos = normalize( (u + v) );
    vec3 uv_neg = normalize( (u - v) );
    vec3 vertex0 = mean_on_plane + uv_pos * ( bbox_diag * 2);
    vec3 vertex1 = mean_on_plane + uv_neg * ( bbox_diag * 2);
    vec3 vertex2 = mean_on_plane - (normalize(uv_pos + uv_neg) * ( bbox_diag * 2));// supertriangle_origin + (u * bbox_diag * 4);

    supertriangle_vertices.resize(9 * flt_size);

    uint32_t counter = 0;
    for (uint32_t i = 0; i < 3; ++i) {
        void* dst = supertriangle_vertices.data() + (counter * flt_size);
        if (have_double) {
            memcpy(dst, &vertex0[i], flt_size);
        } else {
            float tmp = vertex0[i];
            memcpy(dst, &tmp, flt_size);
        }
        counter++;
        // supertriangle_vertices[counter++] = quad_vertex0[i];
    }

    for (uint32_t i = 0; i < 3; ++i) {
        void* dst = supertriangle_vertices.data() + (counter * flt_size);
        if (have_double) {
            memcpy(dst, &vertex1[i], flt_size);
        } else {
            float tmp = vertex1[i];
            memcpy(dst, &tmp, flt_size);
        }
        counter++;
        // supertriangle_vertices[counter++] = quad_vertex1[i];
    }

    for (uint32_t i = 0; i < 3; ++i) {
        void* dst = supertriangle_vertices.data() + (counter * flt_size);
        if (have_double) {
            memcpy(dst, &vertex2[i], flt_size);
        } else {
            float tmp = vertex2[i];
            memcpy(dst, &tmp, flt_size);
        }
        counter++;
        // supertriangle_vertices[counter++] = quad_vertex2[i];
    }

    supertriangle_indices.resize(3);

    supertriangle_indices[0] = 0;
    supertriangle_indices[1] = 1;
    supertriangle_indices[2] = 2;
}

void dispatch_planar_section_impl(
    McContext context,
    McFlags flags,
    const McVoid* pSrcMeshVertices,
    const uint32_t* pSrcMeshFaceIndices,
    const uint32_t* pSrcMeshFaceSizes,
    uint32_t numSrcMeshVertices,
    uint32_t numSrcMeshFaces,
    const McDouble* pNormalVector,
    const McDouble sectionOffset,
    uint32_t numEventsInWaitlist,
    const McEvent* pEventWaitList,
    McEvent* pEvent) noexcept(false)
{
    std::shared_ptr<context_t> context_ptr = g_contexts.find_first_if([=](const std::shared_ptr<context_t> cptr) { return cptr->m_user_handle == context; });

    if (context_ptr == nullptr) {
        throw std::invalid_argument("invalid context");
    }

    std::weak_ptr<context_t> context_weak_ptr(context_ptr);

    // submit the dispatch call to be executed asynchronously and return the future
    // object that will be waited on as an event
    const McEvent event_handle = context_ptr->prepare_and_submit_API_task(
        MC_COMMAND_DISPATCH, numEventsInWaitlist, pEventWaitList,
        [=]() {
            if (!context_weak_ptr.expired()) {
                std::shared_ptr<context_t> context = context_weak_ptr.lock();
                if (context) {
                    std::vector<McChar> supertriangle_vertices;
                    std::vector<McIndex> supertriangle_indices;

                    generate_supertriangle_from_mesh_vertices(
                        supertriangle_vertices,
                        supertriangle_indices,
                        flags,
                        pSrcMeshVertices,
                        numSrcMeshVertices,
                        pNormalVector,
                        sectionOffset,
                        context->get_general_position_enforcement_constant());

                    preproc(
                        context,
                        flags,
                        pSrcMeshVertices,
                        pSrcMeshFaceIndices,
                        pSrcMeshFaceSizes,
                        numSrcMeshVertices,
                        numSrcMeshFaces,
                        (McVoid*)supertriangle_vertices.data(),
                        (McIndex*)&supertriangle_indices[0],
                        nullptr,
                        3,
                        1);
                }
            }
        });

    MCUT_ASSERT(pEvent != nullptr);

    *pEvent = event_handle;
}

void get_connected_components_impl(
    const McContext contextHandle,
    const McConnectedComponentType connectedComponentType,
    const uint32_t numEntries,
    McConnectedComponent* pConnComps,
    uint32_t* numConnComps,
    uint32_t numEventsInWaitlist,
    const McEvent* pEventWaitList,
    McEvent* pEvent)
{
    std::shared_ptr<context_t> context_ptr = g_contexts.find_first_if([=](const std::shared_ptr<context_t> cptr) { return cptr->m_user_handle == contextHandle; });

    if (context_ptr == nullptr) {
        throw std::invalid_argument("invalid context");
    }

    std::weak_ptr<context_t> context_weak_ptr(context_ptr);

    const McEvent event_handle = context_ptr->prepare_and_submit_API_task(
        MC_COMMAND_GET_CONNECTED_COMPONENTS, numEventsInWaitlist, pEventWaitList,
        [=]() {
            if (!context_weak_ptr.expired()) {
                std::shared_ptr<context_t> context = context_weak_ptr.lock();

                if (context) {
                    if (numConnComps != nullptr) {
                        (*numConnComps) = 0; // reset
                    }

                    uint32_t valid_cc_counter = 0;

                    context->connected_components.for_each([&](const std::shared_ptr<connected_component_t> cc_ptr) {
                        const bool is_valid = (cc_ptr->type & connectedComponentType) != 0;

                        if (is_valid) {
                            if (pConnComps == nullptr) // query number
                            {
                                (*numConnComps)++;
                            } else // populate pConnComps
                            {
                                if (valid_cc_counter == numEntries) {
                                    return;
                                }
                                pConnComps[valid_cc_counter] = cc_ptr->m_user_handle;
                                valid_cc_counter += 1;
                            }
                        }
                    });
                }
            }
        });

    *pEvent = event_handle;
}

template <class InputIt, class OutputIt>
OutputIt partial_sum(InputIt first, InputIt last, OutputIt d_first)
{
    if (first == last)
        return d_first;

    typename std::iterator_traits<InputIt>::value_type sum = *first;
    *d_first = sum;

    while (++first != last) {
        sum = sum + *first;
        *++d_first = sum;
    }

    return ++d_first;
}

void triangulate_face(
    //  list of indices which define all triangles that result from the CDT
    std::vector<uint32_t>& cc_face_triangulation,
    const std::shared_ptr<context_t>& context_uptr,
    const uint32_t cc_face_vcount,
    const std::vector<vertex_descriptor_t>& cc_face_vertices,
    const hmesh_t& cc,
    const fd_t cc_face_iter)
{
    //
    // init vars (which we do not want to be re-inititalizing)
    //
    std::vector<vec3> cc_face_vcoords3d;
    cc_face_vcoords3d.resize(cc_face_vcount);

    // NOTE: the elements of this array might be reversed, which occurs
    // when the winding-order/orientation of "cc_face_iter" is flipped
    // due to projection (see call to project_to_2d())
    std::vector<vec2> cc_face_vcoords2d; // resized by project_to_2d(...)
                                         // edge of face, which are used by triangulator as "fixed edges" to
    // constrain the CDT
    std::vector<cdt::edge_t> cc_face_edges;

    // used to check that all indices where used in the triangulation.
    // If any entry is false after finshing triangulation then there will be a hole in the output
    // This is use for sanity checking
    std::vector<bool> cc_face_vtx_to_is_used_flag;
    cc_face_vtx_to_is_used_flag.resize(cc_face_vcount);

    // for each vertex in face: get its coordinates
    for (uint32_t i = 0; i < cc_face_vcount; ++i) {
        cc_face_vcoords2d.clear();
        const vertex_descriptor_t cc_face_vertex_descr = SAFE_ACCESS(cc_face_vertices, i);

        const vec3& coords = cc.vertex(cc_face_vertex_descr);

        SAFE_ACCESS(cc_face_vcoords3d, i) = coords;
    }

    // Project face-vertex coordinates to 2D
    //
    // NOTE: Although we are projecting using the plane normal of
    // the plane, the shape and thus area of the face polygon is
    // unchanged (but the winding order might change!).
    // See definition of "project_to_2d()"
    // =====================================================

    // Maps each vertex in face to the reversed index if the polygon
    // winding order was reversed due to projection to 2D. Otherwise,
    // Simply stores the indices from 0 to N-1
    std::vector<uint32_t> face_to_cdt_vmap(cc_face_vcount);
    std::iota(std::begin(face_to_cdt_vmap), std::end(face_to_cdt_vmap), 0);

    {
        vec3 cc_face_normal_vector;
        double cc_face_plane_eq_dparam; //
        const int largest_component_of_normal = compute_polygon_plane_coefficients(
            cc_face_normal_vector,
            cc_face_plane_eq_dparam,
            cc_face_vcoords3d.data(),
            (int)cc_face_vcount);

        project_to_2d(cc_face_vcoords2d, cc_face_vcoords3d, cc_face_normal_vector, largest_component_of_normal);

        //
        // determine the signed area to check if the 2D face polygon
        // is CW (negative) or CCW (positive)
        //

        double signed_area = 0;

        for (uint32_t i = 0; i < cc_face_vcount - 2; ++i) {
            vec2 cur = cc_face_vcoords2d[i];
            vec2 nxt = cc_face_vcoords2d[(i + 1) % cc_face_vcount];
            vec2 nxtnxt = cc_face_vcoords2d[(i + 2) % cc_face_vcount];
            signed_area += orient2d(cur, nxt, nxtnxt);
        }

        const bool winding_order_flipped_due_to_projection = (signed_area < 0);

        if (winding_order_flipped_due_to_projection) {
            // Reverse the order of points so that they are CCW
            std::reverse(cc_face_vcoords2d.begin(), cc_face_vcoords2d.end());

            // for each vertex index in face
            for (int32_t i = 0; i < (int32_t)cc_face_vcount; ++i) {
                // save reverse index map
                face_to_cdt_vmap[i] = wrap_integer(-(i + 1), 0, cc_face_vcount - 1);
            }
        }
    }

    // Winding order tracker (WOT):
    // We use this halfedge data structure to ensure that the winding-order
    // that is computed by the CDT triangulator is consistent with that
    // of "cc_face_iter".
    // Before triangulation, we populate it with the vertices, (half)edges and
    // faces of the neighbours of "cc_face_iter". This information we will be
    // used to check for proper winding-order when we later insert the CDT
    // triangles whose winding order we assume to be inconsistent with
    // "cc_face_iter"
    hmesh_t wot;
    // vertex descriptor map (from WOT to CC)
    // std::map<vertex_descriptor_t, vertex_descriptor_t> wot_to_cc_vmap;

    // vertex descriptor map (from CC to WOT)
    std::map<vertex_descriptor_t, vertex_descriptor_t> cc_to_wot_vmap;

    // The halfedge with-which we will identify the first CDT triangle to insert into the
    // array "cc_face_triangulation" (see below when we actually do insertion).
    //
    // The order of triangle insertion must priotise the triangle adjacent to the boundary,
    // which are those that are incident to a fixed-edge in the CDT triangulators output.
    // We need "cc_seed_halfedge" to ensure that the first CDT triangle to be inserted is inserted with the
    // correct winding order. This caters to the scenario where "WOT" does not
    // contain enough information to be able to reject the winding-order with which we
    // attempt to insert _the first_ CDT triangle into "cc_face_triangulation".
    //
    // It is perfectly possible for "cc_seed_halfedge" to remain null, which will happen if "cc_face_iter"
    // is the only face in the connected component.
    halfedge_descriptor_t cc_seed_halfedge = hmesh_t::null_halfedge();
    // ... those we have already saved in the wot
    // This is needed to prevent attempting to add the same neighbour face into
    // the WOT, which can happen if the cc_face_iter shares two or more edges
    // with a neighbours (this is possible since our connected components
    // can have n-gon faces )
    std::unordered_set<face_descriptor_t> wot_traversed_neighbours;
    // ... in CCW order
    const std::vector<halfedge_descriptor_t>& cc_face_halfedges = cc.get_halfedges_around_face(cc_face_iter);

    // for each halfedge of face
    for (std::vector<halfedge_descriptor_t>::const_iterator hiter = cc_face_halfedges.begin(); hiter != cc_face_halfedges.end(); ++hiter) {

        halfedge_descriptor_t h = *hiter;
        halfedge_descriptor_t opph = cc.opposite(h);
        face_descriptor_t neigh = cc.face(opph);

        const bool neighbour_exists = (neigh != hmesh_t::null_face());

        // neighbour exists and we have not already traversed it
        // by adding it into the WOT
        if (neighbour_exists && wot_traversed_neighbours.count(neigh) == 0) {

            if (cc_seed_halfedge == hmesh_t::null_halfedge()) {
                cc_seed_halfedge = h; // set once based on first neighbour
            }

            //
            // insert the neighbour into WOT.
            // REMEMBER: the stored connectivity information is what we
            // will use to ensure that we insert triangles into "cc_face_triangulation"
            // with the correct orientation.
            //

            const std::vector<vertex_descriptor_t>& vertices_around_neighbour = cc.get_vertices_around_face(neigh);

            // face vertices (their descriptors for indexing into the WOT)
            std::vector<vertex_descriptor_t> remapped_descrs; // from CC to WOT

            // for each vertex around neighbour
            for (std::vector<vertex_descriptor_t>::const_iterator neigh_viter = vertices_around_neighbour.cbegin();
                 neigh_viter != vertices_around_neighbour.cend(); ++neigh_viter) {

                // Check if vertex is already added into the WOT
                std::map<vertex_descriptor_t, vertex_descriptor_t>::const_iterator cc_to_wot_vmap_iter = cc_to_wot_vmap.find(*neigh_viter);

                if (cc_to_wot_vmap_iter == cc_to_wot_vmap.cend()) { // if not ..

                    const vec3& neigh_vertex_coords = cc.vertex(*neigh_viter);

                    const vertex_descriptor_t woe_vdescr = wot.add_vertex(neigh_vertex_coords);

                    cc_to_wot_vmap_iter = cc_to_wot_vmap.insert(std::make_pair(*neigh_viter, woe_vdescr)).first;
                }

                MCUT_ASSERT(cc_to_wot_vmap_iter != cc_to_wot_vmap.cend());

                remapped_descrs.push_back(cc_to_wot_vmap_iter->second);
            }

            // add the neighbour into WOT
            face_descriptor_t nfd = wot.add_face(remapped_descrs);

            MCUT_ASSERT(nfd != hmesh_t::null_face());
        }

        wot_traversed_neighbours.insert(neigh);
    }

    // Add (remaining) vertices of "cc_face_iter" into WOT.
    //
    // NOTE: some (or all) of the vertices of the "cc_face_iter"
    // might already have been added when registering the neighbours.
    // However, we must still check that all vertices have been added
    // since a vertex is added (during the previous neighbour
    // registration phase) if-and-only-if it is used by a neighbour.
    // Thus vertices are only added during neighbour registration
    // phase if they are incident to an edge that is shared with another
    // face.
    // If "cc_face_iter" has zero neighbours then non of it vertices
    // will have been added in the previous phase.
    // =======================================

    // vertex descriptor map (from CDT to WOT)
    std::map<uint32_t, vertex_descriptor_t> cdt_to_wot_vmap;
    // vertex descriptor map (from WOT to CDT)
    std::map<vertex_descriptor_t, uint32_t> wot_to_cdt_vmap;

    // for each vertex of face
    for (uint32_t i = 0; i < cc_face_vcount; ++i) {

        const vertex_descriptor_t cc_face_vertex_descr = SAFE_ACCESS(cc_face_vertices, i);

        // check if vertex has already been added into the WOT
        std::map<vertex_descriptor_t, vertex_descriptor_t>::const_iterator fiter = cc_to_wot_vmap.find(cc_face_vertex_descr);

        if (fiter == cc_to_wot_vmap.cend()) { // ... if not

            const vec3& coords = SAFE_ACCESS(cc_face_vcoords3d, i);

            vertex_descriptor_t vd = wot.add_vertex(coords);

            fiter = cc_to_wot_vmap.insert(std::make_pair(cc_face_vertex_descr, vd)).first;
        }

        cdt_to_wot_vmap[i] = fiter->second;
        wot_to_cdt_vmap[fiter->second] = i;
    }

    //
    // In the following section, we will check-for and handle
    // the case of having duplicate vertices in "cc_face_iter".
    //
    // Duplicate vertices arise when "cc_face_iter" is from the source-mesh
    // and it has a partial-cut. Example: source-mesh=triangle and
    // cut-mesh=triangle, where the cut-mesh does not split the source-mesh into
    // two disjoint parts (i.e. a triangle and a quad) but instead
    // induces a slit
    //

    // Find the duplicates (if any)
    const cdt::duplicates_info_t duplicates_info_pre = cdt::find_duplicates<double>(
        cc_face_vcoords2d.cbegin(),
        cc_face_vcoords2d.cend(),
        cdt::get_x_coord_vec2d<double>,
        cdt::get_y_coord_vec2d<double>);

    // number of duplicate vertices (if any)
    const uint32_t duplicate_vcount = (uint32_t)duplicates_info_pre.duplicates.size();
    const bool have_duplicates = duplicate_vcount > 0;

    if (have_duplicates) {

        // for each pair of duplicate vertices
        for (std::vector<std::size_t>::const_iterator duplicate_vpair_iter = duplicates_info_pre.duplicates.cbegin();
             duplicate_vpair_iter != duplicates_info_pre.duplicates.cend(); ++duplicate_vpair_iter) {

            //
            // The two vertices are duplicates because they have the _exact_ same coordinates.
            // We make these points unique by perturbing the coordinates of one of them. This requires care
            // because we want to ensure that "cc_face_iter" remains a simple polygon (without
            // self-intersections) after perturbation. To do this, we must perturbation one
            // vertex in the direction that lies on the left-side (i.e. CCW dir) of the two
            // halfedges incident to that vertex. We also take care to account for the fact the two
            // incident edges may be parallel.
            //

            // current duplicate vertex (index in "cc_face_iter")
            const std::int32_t perturbed_dvertex_id = (std::uint32_t)(*duplicate_vpair_iter);
            // previous vertex (in "cc_face_iter") from current duplicate vertex
            const std::uint32_t prev_vtx_id = wrap_integer(perturbed_dvertex_id - 1, 0, cc_face_vcount - 1);
            // next vertex (in "cc_face_iter") from current duplicate vertex
            const std::uint32_t next_vtx_id = wrap_integer(perturbed_dvertex_id + 1, 0, cc_face_vcount - 1);
            // the other duplicate vertex of pair
            const std::int32_t other_dvertex_id = (std::uint32_t)SAFE_ACCESS(duplicates_info_pre.mapping, perturbed_dvertex_id);

            vec2& perturbed_dvertex_coords = SAFE_ACCESS(cc_face_vcoords2d, perturbed_dvertex_id); // will be modified by shifting/perturbation
            const vec2& prev_vtx_coords = SAFE_ACCESS(cc_face_vcoords2d, prev_vtx_id);
            const vec2& next_vtx_coords = SAFE_ACCESS(cc_face_vcoords2d, next_vtx_id);

            // vector along incident edge, pointing from current to previous vertex (NOTE: clockwise dir, reverse)
            const vec2 to_prev = prev_vtx_coords - perturbed_dvertex_coords;
            // vector along incident edge, pointing from current to next vertex (NOTE: counter-clockwise dir, normal)
            const vec2 to_next = next_vtx_coords - perturbed_dvertex_coords;

            //
            // There is a rare case in which MCUT will produce a CC from complete (not partial)! cut where at least
            // one face will be defined by a list of vertices such that this list contains
            // two vertices with exactly the same coordinates (due to limitation of floating point precision e.g. after shewchuk predicates in kernel)
            //
            // In such a case, we "break the tie" by shifting "perturbed_dvertex_id" halfway
            // along the vector running from "perturbed_dvertex_id" to "next_vtx_id",
            // if "perturbed_dvertex_id" and "next_vtx_id" are the duplicates. Otherwise, we shift
            // "perturbed_dvertex_id" halfway
            // along the vector running from "perturbed_dvertex_id" to "prev_vtx_id",
            // if instead "perturbed_dvertex_id" and "prev_vtx_id" are the duplicates.
            //
            // It remains possible that next_vtx_id+1 (prev_vtx_id-1) may also be duplicates, in which
            // case we should just throw our hands up and bail (can be fixed but too hard and rare to justify the effort)
            //
            // These issue can generally be avoided if the input meshes resemble a uniform triangulation
            const bool same_as_prev = ((uint32_t)other_dvertex_id == prev_vtx_id);
            const bool same_as_next = (next_vtx_id == (uint32_t)other_dvertex_id);
            const bool have_adjacent_duplicates = same_as_prev || same_as_next;

            if (have_adjacent_duplicates) {
                // const bool same_as_prev = std::abs(idx_dist_to_prev)==1;
                const vec2& shiftby = (same_as_prev) ? to_next : to_prev;
                // if(same_as_prev)
                //{
                //     shiftby = to_next;
                // }
                // else{ /// then we have "std::abs(idx_dist_to_next) ==1"
                //     shiftby = to_prev;
                // }

                perturbed_dvertex_coords = perturbed_dvertex_coords + (shiftby * 0.5);
            } else { // case of partial cut

                // positive-value if three points are in CCW order (sign_t::ON_POSITIVE_SIDE)
                // negative-value if three points are in CW order (sign_t::ON_NEGATIVE_SIDE)
                // zero if collinear (sign_t::ON_ORIENTED_BOUNDARY)
                const double orient2d_res = orient2d(perturbed_dvertex_coords, next_vtx_coords, prev_vtx_coords);
                const sign_t orient2d_sgn = sign(orient2d_res);

                const double to_prev_sqr_len = squared_length(to_prev);
                const double to_next_sqr_len = squared_length(to_next);

                //
                // Now we must determine which side is the perturbation_vector must be
                // pointing. i.e. the side of "perturbed_dvertex_coords" or the side
                // of its duplicate
                //
                // NOTE: this is only really necessary if the partially cut polygon
                // Has more that 3 intersection points (i.e. more than the case of
                // one tip, and two duplicates)
                //

                const int32_t flip = (orient2d_sgn == sign_t::ON_NEGATIVE_SIDE) ? -1 : 1;

                //
                // Compute the perturbation vector as the average of the two incident edges eminating
                // from the current vertex. NOTE: This perturbation vector should generally point in
                // the direction of the polygon-interior (i.e. analogous to pushing the polygon at
                // the location represented by perturbed_dvertex_coords) to cause a minute dent due to small
                // loss of area.
                // Normalization happens below
                vec2 perturbation_vector = ((to_prev + to_next) / 2.0) * flip;

                // "orient2d()" is exact in the sense that it can depend on computations with numbers
                // whose magnitude is lower than the threshold "orient2d_ccwerrboundA". It follows
                // that this threshold is too "small" a number for us to be able to reliably compute
                // stuff with the result of "orient2d()" that is near this threshold.
                const double errbound = 1e-2;

                // We use "errbound", rather than "orient2d_res", to determine if the incident edges
                // are parallel to give us sufficient room of numerical-precision to reliably compute
                // the perturbation vector.
                // In general, if the incident edges are not parallel then the perturbation vector
                // is computed as the mean of "to_prev" and "to_next". Thus, being "too close"
                // (within some threshold) to the edges being parallel, can induce unpredicatable
                // numerical instabilities, where the mean-vector will be too close to the zero-vector
                // and can complicate the task of perturbation.
                const bool incident_edges_are_parallel = std::fabs(orient2d_res) <= std::fabs(errbound);

                if (incident_edges_are_parallel) {
                    //
                    // pick the shortest of the two incident edges and compute the
                    // orthogonal perturbation vector as the counter-clockwise rotation
                    // of this shortest incident edge.
                    //

                    // flip sign so that the edge is in the CCW dir by pointing from "prev" to "cur"
                    vec2 edge_vec(-to_prev.x(), -to_prev.y());

                    if (to_prev_sqr_len > to_next_sqr_len) {
                        edge_vec = to_next; // pick shortest (NOTE: "to_next" is already in CCW dir)
                    }

                    // rotate the selected edge by 90 degrees
                    const vec2 edge_vec_rotated90(-edge_vec.y(), edge_vec.x());

                    perturbation_vector = edge_vec_rotated90;
                }

                const vec2 perturbation_dir = normalize(perturbation_vector);

                //
                // Compute the maximum length between any two vertices in "cc_face_iter" as the
                // largest length between any two vertices.
                //
                // This will be used to scale "perturbation_dir" so that we find the
                // closest edge (from "perturbed_dvertex_coords") that is intersected by this ray.
                // We will use the resulting information to determine the amount by-which
                // "perturbed_dvertex_coords" is to be perturbed.
                //

                // largest squared length between any two vertices in "cc_face_iter"
                double largest_sqrd_length = -1.0;

                for (uint32_t i = 0; i < cc_face_vcount; ++i) {

                    const vec2& a = SAFE_ACCESS(cc_face_vcoords2d, i);

                    for (uint32_t j = 0; j < cc_face_vcount; ++j) {

                        if (i == j) {
                            continue; // skip -> comparison is redundant
                        }

                        const vec2& b = SAFE_ACCESS(cc_face_vcoords2d, j);

                        const double sqrd_length = squared_length(b - a);
                        largest_sqrd_length = std::max(sqrd_length, largest_sqrd_length);
                    }
                }

                //
                // construct the segment with-which will will find the closest
                // intersection point from "perturbed_dvertex_coords" to "perturbed_dvertex_coords + perturbation_dir*std::sqrt(largest_sqrd_length)"";
                //

                const double shift_len = std::sqrt(largest_sqrd_length);
                const vec2 shift = perturbation_dir * shift_len;

                vec2 intersection_point_on_edge = perturbed_dvertex_coords + shift; // some location potentially outside of polygon

                {
                    struct {
                        vec2 start;
                        vec2 end;
                    } segment;
                    segment.start = perturbed_dvertex_coords;
                    segment.end = perturbed_dvertex_coords + shift;

                    // test segment against all edges to find closest intersection point

                    double segment_min_tval = 1.0;

                    // for each edge of face to be triangulated (number of vertices == number of edges)
                    for (std::uint32_t i = 0; i < cc_face_vcount; ++i) {
                        const std::uint32_t edge_start_idx = i;
                        const std::uint32_t edge_end_idx = (i + 1) % cc_face_vcount;

                        if ((edge_start_idx == (uint32_t)perturbed_dvertex_id || edge_end_idx == (uint32_t)perturbed_dvertex_id) || //
                            (edge_start_idx == (uint32_t)other_dvertex_id || edge_end_idx == (uint32_t)other_dvertex_id)) {
                            continue; // impossible to properly intersect incident edges
                        }

                        const vec2& edge_start_coords = SAFE_ACCESS(cc_face_vcoords2d, edge_start_idx);
                        const vec2& edge_end_coords = SAFE_ACCESS(cc_face_vcoords2d, edge_end_idx);

                        double segment_tval; // parameter along segment
                        double edge_tval; // parameter along current edge
                        vec2 ipoint; // intersection point between segment and current edge

                        const char result = compute_segment_intersection(
                            segment.start, segment.end, edge_start_coords, edge_end_coords,
                            ipoint, segment_tval, edge_tval);

                        if (result == '1' && segment_min_tval > segment_tval) { // we have an clear intersection point
                            segment_min_tval = segment_tval;
                            intersection_point_on_edge = ipoint;
                        } else if (
                            // segment and edge are collinear
                            result == 'e' ||
                            // segment and edge are collinear, or one entity cuts through the vertex of the other
                            result == 'v') {
                            // pick the closest vertex of edge and compute "segment_tval" as a ratio of vector length

                            // length from segment start to the start of edge
                            const double sqr_dist_to_edge_start = squared_length(edge_start_coords - segment.start);
                            // length from segment start to the end of edge
                            const double sqr_dist_to_edge_end = squared_length(edge_end_coords - segment.start);

                            // length from start of segment to either start of edge or end of edge (depending on which is closer)
                            double sqr_dist_to_closest = sqr_dist_to_edge_start;
                            const vec2* ipoint_ptr = &edge_start_coords;

                            if (sqr_dist_to_edge_start > sqr_dist_to_edge_end) {
                                sqr_dist_to_closest = sqr_dist_to_edge_end;
                                ipoint_ptr = &edge_end_coords;
                            }

                            // ratio along segment
                            segment_tval = std::sqrt(sqr_dist_to_closest) / shift_len;

                            if (segment_min_tval > segment_tval) {
                                segment_min_tval = segment_tval;
                                intersection_point_on_edge = *ipoint_ptr; // closest point
                            }
                        }
                    }

                    MCUT_ASSERT(segment_min_tval <= 1.0); // ... because we started from max length between any two vertices
                }

                // Shortened perturbation vector: shortening from the vector that is as long as the
                // max length between any two vertices in "cc_face_iter", to a vector that runs
                // from "perturbed_dvertex_coords" and upto the boundary-point of the "cc_face_iter", along
                // "perturbation_vector" and passing through the interior of "cc_face_iter")
                const vec2 revised_perturbation_vector = (intersection_point_on_edge - perturbed_dvertex_coords);
                const double revised_perturbation_len = length(revised_perturbation_vector);

                const double scale = (errbound * revised_perturbation_len);
                // The translation by which we perturb "perturbed_dvertex_coords"
                //
                // NOTE: since "perturbation_vector" was constructed from "to_prev" and "to_next",
                // "displacement" is by-default pointing in the positive/CCW direction, which is torward
                // the interior of the polygon represented by "cc_face_iter".
                // Thus, the cases with "orient2d_sgn == sign_t::ON_POSITIVE_SIDE" and
                // "orient2d_sgn == sign_t::ON_ORIENTED_BOUNDARY", result in the same displacement vector
                const vec2 displacement = (perturbation_dir * scale);

                // perturb
                perturbed_dvertex_coords = perturbed_dvertex_coords + displacement;

                //} // for (std::uint32_t dv_iter = 0; dv_iter < 2; ++dv_iter) {
            } // if(have_adjacent_duplicates)
        } // for (std::vector<std::size_t>::const_iterator duplicate_vpair_iter = duplicates_info_pre.duplicates.cbegin(); ...
    } // if (have_duplicates) {

    //
    // create the constraint edges for the CDT triangulator, which are just the edges of "cc_face_iter"
    //
    for (uint32_t i = 0; i < cc_face_vcount; ++i) {
        cc_face_edges.push_back(cdt::edge_t(i, (i + 1) % cc_face_vcount));
    }

    // check for duplicate vertices again
    const cdt::duplicates_info_t duplicates_info_post = cdt::find_duplicates<double>(
        cc_face_vcoords2d.cbegin(),
        cc_face_vcoords2d.cend(),
        cdt::get_x_coord_vec2d<double>,
        cdt::get_y_coord_vec2d<double>);

    if (!duplicates_info_post.duplicates.empty()) {
        // This should not happen! Probably a good idea to email the author
        context_uptr->dbg_cb(
            MC_DEBUG_SOURCE_KERNEL,
            MC_DEBUG_TYPE_ERROR, 0,
            MC_DEBUG_SEVERITY_HIGH, "face f" + std::to_string(cc_face_iter) + " has duplicate vertices that could not be resolved (bug)");
        return; // skip to next face (will leave a hole in the output)
    }

    // allocate triangulator
    cdt::triangulator_t<double> cdt(cdt::vertex_insertion_order_t::AS_GIVEN);
    cdt.insert_vertices(cc_face_vcoords2d); // potentially perturbed (if duplicates exist)
    cdt.insert_edges(cc_face_edges);
    cdt.erase_outer_triangles(); // do the constrained delaunay triangulation

    // const std::unordered_map<cdt::edge_t, std::vector<cdt::edge_t>> tmp = cdt::edge_to_pieces_mapping(cdt.pieceToOriginals);
    // const std::unordered_map<cdt::edge_t, std::vector<std::uint32_t>> edgeToSplitVerts = cdt::get_edge_to_split_vertices_map(tmp, cdt.vertices);

    if (!cdt::check_topology(cdt)) {

        context_uptr->dbg_cb(
            MC_DEBUG_SOURCE_KERNEL,
            MC_DEBUG_TYPE_OTHER, 0,
            MC_DEBUG_SEVERITY_NOTIFICATION, "triangulation on face f" + std::to_string(cc_face_iter) + " has invalid topology");

        return; // skip to next face (will leave a hole in the output)
    }

    if (cdt.triangles.empty()) {
        context_uptr->dbg_cb(
            MC_DEBUG_SOURCE_KERNEL,
            MC_DEBUG_TYPE_OTHER, 0,
            MC_DEBUG_SEVERITY_NOTIFICATION, "triangulation on face f" + std::to_string(cc_face_iter) + " produced zero faces");

        return; // skip to next face (will leave a hole in the output)
    }

    //
    // In the following, we will now save the produce triangles into the
    // output array "cc_face_triangulation".
    //

    // number of CDT triangles
    const uint32_t cc_face_triangle_count = (uint32_t)cdt.triangles.size();

    //
    // We insert triangles into "cc_face_triangulation" by using a
    // breadth-first search-like flood-fill strategy to "walk" the
    // triangles of the CDT. We start from a prescribed triangle next to the
    // boundary of "cc_face_iter".
    //

    // map vertices to CDT triangles
    // Needed for the BFS traversal of triangles
    std::vector<std::vector<uint32_t>> vertex_to_triangle_map(cc_face_vcount, std::vector<uint32_t>());

    // for each CDT triangle
    for (uint32_t i = 0; i < cc_face_triangle_count; ++i) {

        const cdt::triangle_t& triangle = SAFE_ACCESS(cdt.triangles, i);

        // for each triangle vertex
        for (uint32_t j = 0; j < 3; j++) {
            const uint32_t cdt_vertex_id = SAFE_ACCESS(triangle.vertices, j);
            const uint32_t cc_face_vertex_id = SAFE_ACCESS(face_to_cdt_vmap, cdt_vertex_id);
            std::vector<uint32_t>& incident_triangles = SAFE_ACCESS(vertex_to_triangle_map, cc_face_vertex_id);
            incident_triangles.push_back(i); // save mapping
        }
    }

    // start with any boundary edge (AKA constraint/fixed edge)
    std::unordered_set<cdt::edge_t>::const_iterator fixed_edge_iter = cdt.fixedEdges.cbegin();

    // NOTE: in the case that "cc_seed_halfedge" is null, then "cc_face_iter"
    // is the only face in its connected component (mesh) and therefore
    // it has no neighbours. In this case, the winding-order of the produced triangles
    // is dependent on the CDT triangulator. The MCUT frontend will at best be able to
    // ensure that all CDT triangles have consistent winding order (even if the triangulator
    // produced mixed winding orders between the resulting triangles) but we cannot guarrantee
    // the "front-facing" side of triangulated "cc_face_iter" will match that of its
    // original non-triangulated form from the connected component.
    //
    // We leave that to the user to fix upon visual inspection.
    //
    const bool have_seed_halfedge = cc_seed_halfedge != hmesh_t::null_halfedge();

    if (have_seed_halfedge) {
        // if the seed halfedge exists then the triangulated face must have
        // atleast one neighbour
        MCUT_ASSERT(wot.number_of_faces() != 0);

        // source and target descriptor in the connected component
        const vertex_descriptor_t cc_seed_halfedge_src = cc.source(cc_seed_halfedge);
        const vertex_descriptor_t cc_seed_halfedge_tgt = cc.target(cc_seed_halfedge);

        // source and target descriptor in the face
        const vertex_descriptor_t woe_src = SAFE_ACCESS(cc_to_wot_vmap, cc_seed_halfedge_src);
        const uint32_t cdt_src = SAFE_ACCESS(wot_to_cdt_vmap, woe_src);
        const vertex_descriptor_t woe_tgt = SAFE_ACCESS(cc_to_wot_vmap, cc_seed_halfedge_tgt);
        const uint32_t cdt_tgt = SAFE_ACCESS(wot_to_cdt_vmap, woe_tgt);

        // find the fixed edge in the CDT matching the vertices of the seed halfedge

        fixed_edge_iter = std::find_if(
            cdt.fixedEdges.cbegin(),
            cdt.fixedEdges.cend(),
            [&](const cdt::edge_t& e) -> bool {
                return (e.v1() == cdt_src && e.v2() == cdt_tgt) || //
                    (e.v2() == cdt_src && e.v1() == cdt_tgt);
            });

        MCUT_ASSERT(fixed_edge_iter != cdt.fixedEdges.cend());
    }

    // must always exist since cdt edge ultimately came from the CC, and also
    // due to the fact that we have inserted edges into the CDT
    MCUT_ASSERT(fixed_edge_iter != cdt.fixedEdges.cend());

    // get the two vertices of the "seed" fixed edge (indices into CDT)
    const std::uint32_t fixed_edge_vtx0_id = fixed_edge_iter->v1();
    const std::uint32_t fixed_edge_vtx1_id = fixed_edge_iter->v2();

    //
    // Since these vertices share an edge, they will share a triangle in the CDT
    // So lets get that shared triangle, which will be the seed triangle for the
    // traversal process, which we will use to walk and insert triangles into
    // the output array "cc_face_triangulation"
    //

    // incident triangles of first vertex
    const std::vector<std::uint32_t>& fixed_edge_vtx0_tris = SAFE_ACCESS(vertex_to_triangle_map, fixed_edge_vtx0_id);
    MCUT_ASSERT(fixed_edge_vtx0_tris.empty() == false);
    // incident triangles of second vertex
    const std::vector<std::uint32_t>& fixed_edge_vtx1_tris = SAFE_ACCESS(vertex_to_triangle_map, fixed_edge_vtx1_id);
    MCUT_ASSERT(fixed_edge_vtx1_tris.empty() == false);

    // the shared triangle between the two vertices of fixed edge
    std::uint32_t fix_edge_seed_triangle = cdt::null_neighbour;

    // for each CDT triangle incident to the first vertex
    for (std::vector<std::uint32_t>::const_iterator it = fixed_edge_vtx0_tris.begin(); it != fixed_edge_vtx0_tris.end(); ++it) {

        if (*it == cdt::null_neighbour) {
            continue;
        }

        // does it exist in the incident triangle list of the other vertex?
        if (std::find(fixed_edge_vtx1_tris.begin(), fixed_edge_vtx1_tris.end(), *it) != fixed_edge_vtx1_tris.end()) {
            fix_edge_seed_triangle = *it; // found
            break; // done
        }
    }

    MCUT_ASSERT(fix_edge_seed_triangle != cdt::null_neighbour);

    std::stack<std::uint32_t> seeds(std::deque<std::uint32_t>(1, fix_edge_seed_triangle));

    // collection of traversed CDT triangles
    std::unordered_set<std::uint32_t> traversed;

    while (!seeds.empty()) { // while we still have triangles to walk

        const std::uint32_t curr_triangle_id = seeds.top();
        seeds.pop();

        traversed.insert(curr_triangle_id); // those we have walked

        const cdt::triangle_t& triangle = cdt.triangles[curr_triangle_id];

        //
        // insert current triangle into our triangulated CC mesh
        //
        const uint32_t triangle_vertex_count = 3;

        // from CDT/"cc_face_iter" indices to WOT descriptors
        std::vector<vertex_descriptor_t> remapped_triangle(triangle_vertex_count, hmesh_t::null_vertex());

        // for each vertex of triangle
        for (uint32_t i = 0; i < triangle_vertex_count; i++) {
            // index of current vertex in CDT/"cc_face_iter"
            const uint32_t cdt_vertex_id = SAFE_ACCESS(triangle.vertices, i);
            const uint32_t cc_face_vertex_id = SAFE_ACCESS(face_to_cdt_vmap, cdt_vertex_id);

            // mark vertex as used (for sanity check)
            SAFE_ACCESS(cc_face_vtx_to_is_used_flag, cc_face_vertex_id) = true;

            // remap triangle vertex index
            SAFE_ACCESS(remapped_triangle, i) = SAFE_ACCESS(cdt_to_wot_vmap, cc_face_vertex_id);

            // save index into output array (where every three indices is a triangle)
            cc_face_triangulation.emplace_back(cc_face_vertex_id);
        }

        // check that the winding order respects the winding order of "cc_face_iter"
        const bool is_insertible = wot.is_insertable(remapped_triangle);

        if (!is_insertible) { // CDT somehow produce a triangle with reversed winding-order (i.e. CW)

            // flip the winding order by simply swapping indices
            uint32_t a = remapped_triangle[0];
            uint32_t c = remapped_triangle[2];
            std::swap(a, c); // swap indices in the remapped triangle
            remapped_triangle[0] = vertex_descriptor_t(a);
            remapped_triangle[2] = vertex_descriptor_t(c);
            const size_t N = cc_face_triangulation.size();

            // swap indice in the saved triangle
            std::swap(cc_face_triangulation[N - 1], cc_face_triangulation[N - 3]); // reverse last added triangle's indices
        }

        // add face into our WO wot
        face_descriptor_t fd = wot.add_face(remapped_triangle); // keep track of added triangles from CDT

        // if this happens then CDT gave us a strange triangulation e.g. duplicate triangles with opposite winding order
        if (fd == hmesh_t::null_face()) {
            // Simply remove/ignore the offending triangle. We cannot do anything at this stage.
            cc_face_triangulation.pop_back();
            cc_face_triangulation.pop_back();
            cc_face_triangulation.pop_back();

            const std::string msg = "triangulation on face f" + std::to_string(cc_face_iter) + " produced invalid triangles that could not be stored";

            context_uptr->dbg_cb(
                MC_DEBUG_SOURCE_KERNEL,
                MC_DEBUG_TYPE_OTHER, 0,
                MC_DEBUG_SEVERITY_HIGH, msg);
        }

        //
        // We will now add the neighbouring CDT triangles into queue/stack
        //

        // for each CDT vertex
        for (std::uint32_t i(0); i < triangle_vertex_count; ++i) {

            const uint32_t next = triangle.vertices[cdt::ccw(i)];
            const uint32_t prev = triangle.vertices[cdt::cw(i)];
            const cdt::edge_t query_edge(next, prev);

            if (cdt.fixedEdges.count(query_edge)) {
                continue; // current edge is fixed edge so there is no neighbour
            }

            const std::uint32_t neighbour_index = triangle.neighbors[cdt::get_opposite_neighbour_from_vertex(i)];

            if (neighbour_index != cdt::null_neighbour && traversed.count(neighbour_index) == 0) {
                seeds.push(neighbour_index);
            }
        }
    } // while (!seeds.empty()) {

    // every triangle in the finalized CDT must be walked!
    MCUT_ASSERT(traversed.size() == cdt.triangles.size()); // this might be violated if CDT produced duplicate triangles

    //
    // Final sanity check
    //

    for (std::uint32_t i = 0; i < (std::uint32_t)cc_face_vcount; ++i) {
        if (SAFE_ACCESS(cc_face_vtx_to_is_used_flag, i) != true) {
            context_uptr->dbg_cb(
                MC_DEBUG_SOURCE_KERNEL,
                MC_DEBUG_TYPE_OTHER, 0,
                MC_DEBUG_SEVERITY_HIGH, "triangulation on face f" + std::to_string(cc_face_iter) + " did not use vertex v" + std::to_string(i));
        }
    }
}

/* Helper function (to prevent code duplication), which maps a given index of
 an internal-inputmesh face, to it corresponding face-index value in the user-provided
 input-mesh.

 This is needed because MCUT may modify the user-inputmesh (source mesh or cut mesh)
 when it performs polygon partitioning (subject to teh nature of the intersection).

 Refer to the comments about polygon partitioning
  */
uint32_t map_internal_inputmesh_face_idx_to_user_inputmesh_face_idx(
    const uint32_t internal_inputmesh_face_idx,
    const std::shared_ptr<connected_component_t>& cc_uptr)
{
    // uint32_t internal_inputmesh_face_idx = (uint32_t)cc_uptr->kernel_hmesh_data->data_maps.face_map[i];
    uint32_t user_inputmesh_face_idx = INT32_MAX; // return value
    const bool internal_input_mesh_face_idx_is_for_src_mesh = (internal_inputmesh_face_idx < cc_uptr->internal_sourcemesh_face_count);

    if (internal_input_mesh_face_idx_is_for_src_mesh) {

        std::unordered_map<fd_t, fd_t>::const_iterator fiter = cc_uptr->source_hmesh_child_to_usermesh_birth_face->find(fd_t(internal_inputmesh_face_idx));

        if (fiter != cc_uptr->source_hmesh_child_to_usermesh_birth_face->cend()) {
            user_inputmesh_face_idx = fiter->second;
        } else {
            user_inputmesh_face_idx = internal_inputmesh_face_idx;
        }
        MCUT_ASSERT(user_inputmesh_face_idx < cc_uptr->client_sourcemesh_face_count);
    } else // internalInputMeshVertexDescrIsForCutMesh
    {
        std::unordered_map<fd_t, fd_t>::const_iterator fiter = cc_uptr->cut_hmesh_child_to_usermesh_birth_face->find(fd_t(internal_inputmesh_face_idx));

        if (fiter != cc_uptr->cut_hmesh_child_to_usermesh_birth_face->cend()) {
            uint32_t unoffsettedDescr = (fiter->second - cc_uptr->internal_sourcemesh_face_count);
            user_inputmesh_face_idx = unoffsettedDescr + cc_uptr->client_sourcemesh_face_count;
        } else {
            uint32_t unoffsettedDescr = (internal_inputmesh_face_idx - cc_uptr->internal_sourcemesh_face_count);
            user_inputmesh_face_idx = unoffsettedDescr + cc_uptr->client_sourcemesh_face_count;
        }
    }

    return user_inputmesh_face_idx;
}

void get_connected_component_data_impl_detail(
    std::shared_ptr<context_t> context_ptr,
    const McConnectedComponent connCompId,
    McFlags flags,
    McSize bytes,
    McVoid* pMem,
    McSize* pNumBytes)
{
#if 0
    std::map<McContext, std::unique_ptr<context_t>>::iterator context_entry_iter = g_contexts.find(context);

    if (context_entry_iter == g_contexts.end()) {
        throw std::invalid_argument("invalid context");
    }

    std::unique_ptr<context_t>& context_uptr = context_entry_iter->second;

    std::map<McConnectedComponent, std::shared_ptr<connected_component_t>>::iterator cc_entry_iter = context_uptr->connected_components.find(connCompId);

    if (cc_entry_iter == context_uptr->connected_components.cend()) {
        throw std::invalid_argument("invalid connected component");
    }

    std::shared_ptr<connected_component_t>& cc_uptr = cc_entry_iter->second;
#endif

    std::shared_ptr<connected_component_t> cc_uptr = context_ptr->connected_components.find_first_if([=](const std::shared_ptr<connected_component_t> ccptr) { return ccptr->m_user_handle == connCompId; });
    if (!cc_uptr) {
        throw std::invalid_argument("invalid connected component");
    }
    switch (flags) {

    case MC_CONNECTED_COMPONENT_DATA_VERTEX_FLOAT: {
        SCOPED_TIMER("MC_CONNECTED_COMPONENT_DATA_VERTEX_FLOAT");

        const McSize allocated_bytes = cc_uptr->kernel_hmesh_data->mesh->number_of_vertices() * sizeof(float) * 3ul; // cc_uptr->indexArrayMesh.numVertices * sizeof(float) * 3;

        if (pMem == nullptr) {
            *pNumBytes = allocated_bytes;
        } else { // copy mem to client ptr

            if (bytes > allocated_bytes) {
                throw std::invalid_argument("out of bounds memory access");
            } // if

            // an element is a component
            const McSize nelems = (McSize)(bytes / sizeof(float));

            if (nelems % 3 != 0) {
                throw std::invalid_argument("invalid number of bytes");
            }

            const McSize num_vertices_to_copy = (nelems / 3);

            float* casted_ptr = reinterpret_cast<float*>(pMem);

#if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
            {

                auto fn_copy_vertex_coords = [&casted_ptr, &cc_uptr, &num_vertices_to_copy](vertex_array_iterator_t block_start_, vertex_array_iterator_t block_end_) {
                    // thread starting offset (in vertex count) in the "array of vertices"
                    const McSize base_offset = std ::distance(cc_uptr->kernel_hmesh_data->mesh->vertices_begin(), block_start_);

                    McSize elem_offset = base_offset * 3;

                    for (vertex_array_iterator_t vertex_iter = block_start_; vertex_iter != block_end_; ++vertex_iter) {

                        if (((elem_offset + 1) / 3) == num_vertices_to_copy) {
                            break; // reach what the user asked for
                        }

                        const vertex_descriptor_t descr = *vertex_iter;
                        const vec3& coords = cc_uptr->kernel_hmesh_data->mesh->vertex(descr);

                        // for each component of coordinate
                        for (int i = 0; i < 3; ++i) {
                            const float val = static_cast<float>(coords[i]);
                            *(casted_ptr + elem_offset) = val;
                            elem_offset += 1;
                        }
                    }
                };

                parallel_for(
                    context_ptr->get_shared_compute_threadpool(),
                    cc_uptr->kernel_hmesh_data->mesh->vertices_begin(),
                    cc_uptr->kernel_hmesh_data->mesh->vertices_end(),
                    fn_copy_vertex_coords);
            }
#else // #if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
            McSize elem_offset = 0;
            for (vertex_array_iterator_t viter = cc_uptr->kernel_hmesh_data->mesh->vertices_begin(); viter != cc_uptr->kernel_hmesh_data->mesh->vertices_end(); ++viter) {
                const vec3& coords = cc_uptr->kernel_hmesh_data->mesh->vertex(*viter);

                for (int i = 0; i < 3; ++i) {
                    const float val = static_cast<float>(coords[i]);
                    *(casted_ptr + elem_offset) = val;
                    elem_offset += 1;
                }

                if ((elem_offset / 3) == num_vertices_to_copy) {
                    break;
                }
            }

            MCUT_ASSERT((elem_offset * sizeof(float)) <= allocated_bytes);
#endif // #if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
        }
    } break;
    case MC_CONNECTED_COMPONENT_DATA_VERTEX_DOUBLE: {
        SCOPED_TIMER("MC_CONNECTED_COMPONENT_DATA_VERTEX_DOUBLE");
        const McSize allocated_bytes = cc_uptr->kernel_hmesh_data->mesh->number_of_vertices() * sizeof(double) * 3ul; // cc_uptr->indexArrayMesh.numVertices * sizeof(float) * 3;

        if (pMem == nullptr) {
            *pNumBytes = allocated_bytes;
        } else { // copy mem to client ptr

            if (bytes > allocated_bytes) {
                throw std::invalid_argument("out of bounds memory access");
            } // if

            // an element is a component
            const int64_t nelems = (McSize)(bytes / sizeof(double));

            if (nelems % 3 != 0) {
                throw std::invalid_argument("invalid number of bytes");
            }

            const McSize num_vertices_to_copy = (nelems / 3);

            double* casted_ptr = reinterpret_cast<double*>(pMem);

#if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
            {
                typedef vertex_array_iterator_t InputStorageIteratorType;

                auto fn_copy_vertex_coords = [&casted_ptr, &cc_uptr, &num_vertices_to_copy](vertex_array_iterator_t block_start_, vertex_array_iterator_t block_end_) {
                    // thread starting offset (in vertex count) in the "array of vertices"
                    const McSize base_offset = std ::distance(cc_uptr->kernel_hmesh_data->mesh->vertices_begin(), block_start_);

                    McSize elem_offset = base_offset * 3;

                    for (InputStorageIteratorType vertex_iter = block_start_; vertex_iter != block_end_; ++vertex_iter) {

                        if ((elem_offset / 3) == num_vertices_to_copy) {
                            break; // reach what the user asked for
                        }

                        const vertex_descriptor_t descr = *vertex_iter;
                        const vec3& coords = cc_uptr->kernel_hmesh_data->mesh->vertex(descr);

                        // for each component of coordinate
                        for (int i = 0; i < 3; ++i) {
                            const double val = static_cast<double>(coords[i]);
                            *(casted_ptr + elem_offset) = val;
                            elem_offset += 1;
                        }
                    }
                };

                parallel_for(
                    context_ptr->get_shared_compute_threadpool(),
                    cc_uptr->kernel_hmesh_data->mesh->vertices_begin(),
                    cc_uptr->kernel_hmesh_data->mesh->vertices_end(),
                    fn_copy_vertex_coords);
            }
#else // #if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
            McSize elem_offset = 0;
            for (vertex_array_iterator_t viter = cc_uptr->kernel_hmesh_data->mesh->vertices_begin(); viter != cc_uptr->kernel_hmesh_data->mesh->vertices_end(); ++viter) {

                const vec3& coords = cc_uptr->kernel_hmesh_data->mesh->vertex(*viter);

                for (int i = 0; i < 3; ++i) {
                    *(casted_ptr + elem_offset) = coords[i];
                    elem_offset += 1;
                }

                if ((elem_offset / 3) == num_vertices_to_copy) {
                    break;
                }
            }

            MCUT_ASSERT((elem_offset * sizeof(float)) <= allocated_bytes);
#endif // #if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
        }
    } break;
    case MC_CONNECTED_COMPONENT_DATA_DISPATCH_PERTURBATION_VECTOR: {
        SCOPED_TIMER("MC_CONNECTED_COMPONENT_DATA_DISPATCH_PERTURBATION_VECTOR");
        if (pMem == nullptr) {
            *pNumBytes = sizeof(vec3);
        } else {
            if (bytes > sizeof(vec3)) {
                throw std::invalid_argument("out of bounds memory access");
            }
            if (bytes % sizeof(vec3) != 0) {
                throw std::invalid_argument("invalid number of bytes");
            }

            ((McDouble*)pMem)[0] = cc_uptr->perturbation_vector[0];
            ((McDouble*)pMem)[1] = cc_uptr->perturbation_vector[1];
            ((McDouble*)pMem)[2] = cc_uptr->perturbation_vector[2];
        }
    } break;
    case MC_CONNECTED_COMPONENT_DATA_FACE: {
        SCOPED_TIMER("MC_CONNECTED_COMPONENT_DATA_FACE");
        if (pMem == nullptr) { // querying for number of bytes
            uint32_t num_indices = 0;

#if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
            {
                // each worker-thread will count the number of indices according
                // to the number of faces in its range/block. The master thread
                // will then sum the total from all threads

                // typedef std::tuple<uint32_t> OutputStorageTypesTuple; // store number of indices computed by worker
                typedef face_array_iterator_t InputStorageIteratorType;

                auto fn_count_indices = [&cc_uptr](InputStorageIteratorType block_start_, InputStorageIteratorType block_end_) {
                    uint32_t num_indices_LOCAL = 0;

                    // thread starting offset (in vertex count) in the "array of vertices"
                    // const McSize face_base_offset = std::distance(cc_uptr->kernel_hmesh_data->mesh->faces_begin(), block_start_);

                    for (InputStorageIteratorType fiter = block_start_; fiter != block_end_; ++fiter) {

                        const uint32_t num_vertices_around_face = cc_uptr->kernel_hmesh_data->mesh->get_num_vertices_around_face(*fiter);

                        MCUT_ASSERT(num_vertices_around_face >= 3);

                        num_indices_LOCAL += num_vertices_around_face;
                    }

                    return num_indices_LOCAL;
                };

                std::vector<std::future<uint32_t>> futures;
                uint32_t partial_res;

                parallel_for(
                    context_ptr->get_shared_compute_threadpool(),
                    cc_uptr->kernel_hmesh_data->mesh->faces_begin(),
                    cc_uptr->kernel_hmesh_data->mesh->faces_end(),
                    fn_count_indices,
                    partial_res, // output computed by master thread
                    futures);

                const uint32_t& num_indices_MASTER_THREAD_LOCAL = partial_res;
                num_indices += num_indices_MASTER_THREAD_LOCAL;

                // wait for all worker-threads to finish copies
                for (uint32_t i = 0; i < (uint32_t)futures.size(); ++i) {
                    const uint32_t num_indices_THREAD_LOCAL = futures[i].get();
                    num_indices += num_indices_THREAD_LOCAL;
                }
            }
#else // #if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
            for (face_array_iterator_t fiter = cc_uptr->kernel_hmesh_data->mesh->faces_begin(); fiter != cc_uptr->kernel_hmesh_data->mesh->faces_end(); ++fiter) {
                const uint32_t num_vertices_around_face = cc_uptr->kernel_hmesh_data->mesh->get_num_vertices_around_face(*fiter);

                MCUT_ASSERT(num_vertices_around_face >= 3);

                num_indices += num_vertices_around_face;
            }

#endif // #if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
            MCUT_ASSERT(num_indices >= 3); // min is a triangle

            *pNumBytes = num_indices * sizeof(uint32_t);
        } else { // querying for data to copy back to user pointer
            if (bytes % sizeof(uint32_t) != 0) {
                throw std::invalid_argument("invalid number of bytes");
            }

            const uint32_t num_indices_to_copy = bytes / sizeof(uint32_t);
            uint32_t* casted_ptr = reinterpret_cast<uint32_t*>(pMem);

#if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
            {
                // step 1: compute array storing face sizes (recursive API call)
                // - for computing exclusive sum
                // step 2: compute exclusive sum array ( in spirit of std::exclusive_scan)
                // - for determining per-thread (work-block) output-array offsets
                // step 3: copy face indices into output array using offsets from previous steps
                // - final result that is stored in user-output array

                const uint32_t nfaces = cc_uptr->kernel_hmesh_data->mesh->number_of_faces();

                //
                // step 1
                //

                // If client already called mcGetConnectedComponentData(..., MC_CONNECTED_COMPONENT_DATA_FACE_SIZE, numBytes, faceSizes.data(), NULL)
                // then we should have already cached the array of face sizes

                if (!cc_uptr->face_sizes_cache_initialized) { // fill the cache by calling the API, within the API!

                    // this is like resizing output array in the client application, after knowing the number of faces in CC
                    cc_uptr->face_sizes_cache.resize(nfaces);

                    const std::size_t num_bytes = nfaces * sizeof(uint32_t);

                    // recursive Internal API call: populate cache here, which also sets "cc_uptr->face_sizes_cache_initialized" to true
                    get_connected_component_data_impl_detail(context_ptr, connCompId, MC_CONNECTED_COMPONENT_DATA_FACE_SIZE, num_bytes, cc_uptr->face_sizes_cache.data(), NULL);
                } else { // cache already initialized
                    MCUT_ASSERT(cc_uptr->face_sizes_cache.empty() == false);
                    MCUT_ASSERT(cc_uptr->face_sizes_cache_initialized == true);
                }

                //
                // step 2
                //
                std::vector<uint32_t> partial_sum_vec = cc_uptr->face_sizes_cache; // copy

                parallel_partial_sum(context_ptr->get_shared_compute_threadpool(), partial_sum_vec.begin(), partial_sum_vec.end());
                const bool flip_winding_order = context_ptr->get_connected_component_winding_order() == McConnectedComponentFaceWindingOrder::MC_CONNECTED_COMPONENT_FACE_WINDING_ORDER_REVERSED;

                //
                // step 3
                //

                auto fn_face_indices_copy = [flip_winding_order, &cc_uptr, &partial_sum_vec, &casted_ptr, &num_indices_to_copy](face_array_iterator_t block_start_, face_array_iterator_t block_end_) {
                    const uint32_t base_face_offset = std::distance(cc_uptr->kernel_hmesh_data->mesh->faces_begin(), block_start_);

                    MCUT_ASSERT(base_face_offset < (uint32_t)cc_uptr->face_sizes_cache.size());

                    // the first face in the range between block start and end
                    const uint32_t base_face_vertex_count = SAFE_ACCESS(cc_uptr->face_sizes_cache, base_face_offset);

                    const uint32_t partial_sum_vec_val = SAFE_ACCESS(partial_sum_vec, base_face_offset);
                    const uint32_t index_arr_base_offset = partial_sum_vec_val - base_face_vertex_count;
                    uint32_t index_arr_offset = index_arr_base_offset;

                    std::vector<vd_t> vertices_around_face; // tmp to prevent reallocations
                    vertices_around_face.reserve(3);

                    for (face_array_iterator_t f_iter = block_start_; f_iter != block_end_; ++f_iter) {

                        vertices_around_face.clear();
                        cc_uptr->kernel_hmesh_data->mesh->get_vertices_around_face(vertices_around_face, *f_iter);
                        const uint32_t num_vertices_around_face = (uint32_t)vertices_around_face.size();

                        MCUT_ASSERT(num_vertices_around_face >= 3u);

                        if (flip_winding_order) {
                            // for each vertex in face
                            for (int32_t i = (num_vertices_around_face - 1); i >= (int32_t)0; --i) {
                                const uint32_t vertex_idx = (uint32_t)SAFE_ACCESS(vertices_around_face, i);
                                *(casted_ptr + index_arr_offset) = vertex_idx;
                                ++index_arr_offset;

                                if (index_arr_offset == num_indices_to_copy) {
                                    break;
                                }
                            }
                        } else {
                            // for each vertex in face
                            for (uint32_t i = 0; i < num_vertices_around_face; ++i) {
                                const uint32_t vertex_idx = (uint32_t)SAFE_ACCESS(vertices_around_face, i);
                                *(casted_ptr + index_arr_offset) = vertex_idx;
                                ++index_arr_offset;

                                if (index_arr_offset == num_indices_to_copy) {
                                    break;
                                }
                            }
                        }
                    }
                };

                parallel_for(
                    context_ptr->get_shared_compute_threadpool(),
                    cc_uptr->kernel_hmesh_data->mesh->faces_begin(),
                    cc_uptr->kernel_hmesh_data->mesh->faces_end(),
                    fn_face_indices_copy,
                    (1 << 14)); // blocks until all work is done
            }
#else // #if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)

            std::vector<vd_t> cc_face_vertices;
            uint32_t elem_offset = 0;

            const bool flip_winding_order = context_ptr->get_connected_component_winding_order() == McConnectedComponentFaceWindingOrder::MC_CONNECTED_COMPONENT_FACE_WINDING_ORDER_REVERSED;

            for (face_array_iterator_t fiter = cc_uptr->kernel_hmesh_data->mesh->faces_begin(); fiter != cc_uptr->kernel_hmesh_data->mesh->faces_end(); ++fiter) {

                cc_face_vertices.clear();
                cc_uptr->kernel_hmesh_data->mesh->get_vertices_around_face(cc_face_vertices, *fiter);
                const uint32_t num_vertices_around_face = (uint32_t)cc_face_vertices.size();

                MCUT_ASSERT(num_vertices_around_face >= 3u);

                if (flip_winding_order) {
                    for (int32_t i = (int32_t)(num_vertices_around_face - 1); i >= 0; --i) {
                        const uint32_t vertex_idx = (uint32_t)cc_face_vertices[i];
                        *(casted_ptr + elem_offset) = vertex_idx;
                        ++elem_offset;

                        if (elem_offset == num_indices_to_copy) {
                            break;
                        }
                    }
                } else {
                    for (uint32_t i = 0; i < num_vertices_around_face; ++i) {
                        const uint32_t vertex_idx = (uint32_t)cc_face_vertices[i];
                        *(casted_ptr + elem_offset) = vertex_idx;
                        ++elem_offset;

                        if (elem_offset == num_indices_to_copy) {
                            break;
                        }
                    }
                }
            }

            MCUT_ASSERT(elem_offset == num_indices_to_copy);
#endif // #if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)

        } // if (pMem == nullptr) { // querying for number of bytes
    } break;
    case MC_CONNECTED_COMPONENT_DATA_FACE_SIZE: { // non-triangulated only (don't want to store redundant information)
        SCOPED_TIMER("MC_CONNECTED_COMPONENT_DATA_FACE_SIZE");
        if (pMem == nullptr) {
            *pNumBytes = cc_uptr->kernel_hmesh_data->mesh->number_of_faces() * sizeof(uint32_t); // each face has a size (num verts)
        } else {
            if (bytes > cc_uptr->kernel_hmesh_data->mesh->number_of_faces() * sizeof(uint32_t)) {
                throw std::invalid_argument("out of bounds memory access");
            }

            if (bytes % sizeof(uint32_t) != 0) {
                throw std::invalid_argument("invalid number of bytes");
            }

            uint32_t* casted_ptr = reinterpret_cast<uint32_t*>(pMem);

#if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
            {
                const uint32_t face_count = cc_uptr->kernel_hmesh_data->mesh->number_of_faces();

                if (!cc_uptr->face_sizes_cache_initialized) { // init cache by storing data into it
                    // the code execution can also reach here because we asked for the cache
                    // to be populated in order to compute/copy the face indices in parallel.
                    // see: MC_CONNECTED_COMPONENT_DATA_FACE case
                    const bool cache_allocated_prior = !cc_uptr->face_sizes_cache.empty();

                    if (!cache_allocated_prior) {
                        cc_uptr->face_sizes_cache.resize(face_count);
                    }

                    auto fn_face_size = [&cc_uptr](std::vector<uint32_t>::iterator block_start_, std::vector<uint32_t>::iterator block_end_) {
                        const uint32_t face_base_offset = (uint32_t)std::distance(cc_uptr->face_sizes_cache.begin(), block_start_);
                        uint32_t face_offset = face_base_offset;

                        for (std::vector<uint32_t>::iterator fs_iter = block_start_; fs_iter != block_end_; ++fs_iter) {

                            const face_descriptor_t descr(face_offset);

                            const uint32_t num_vertices_around_face = cc_uptr->kernel_hmesh_data->mesh->get_num_vertices_around_face(descr);

                            MCUT_ASSERT(num_vertices_around_face >= 3);

                            *fs_iter = num_vertices_around_face;

                            face_offset++;
                        }
                    };

                    parallel_for(
                        context_ptr->get_shared_compute_threadpool(),
                        cc_uptr->face_sizes_cache.begin(),
                        cc_uptr->face_sizes_cache.end(),
                        fn_face_size); // blocks until all work is done

                    cc_uptr->face_sizes_cache_initialized = true;
                }

                // the pointers are different if "cc_uptr->face_sizes_cache" is not
                // being populated in the current call
                const McVoid* src_ptr = reinterpret_cast<McVoid*>(&(cc_uptr->face_sizes_cache[0]));
                const McVoid* dst_ptr = pMem;
                const bool writing_to_client_pointer = (src_ptr != dst_ptr);

                if (writing_to_client_pointer) // copy only if "casted_ptr" is client pointer
                {
                    // copy to user pointer
                    memcpy(casted_ptr, &(cc_uptr->face_sizes_cache[0]), sizeof(uint32_t) * face_count);
                }
            }
#else
            //
            McSize elem_offset = 0;

            for (face_array_iterator_t fiter = cc_uptr->kernel_hmesh_data->mesh->faces_begin(); fiter != cc_uptr->kernel_hmesh_data->mesh->faces_end(); ++fiter) {
                const uint32_t num_vertices_around_face = cc_uptr->kernel_hmesh_data->mesh->get_num_vertices_around_face(*fiter);

                MCUT_ASSERT(num_vertices_around_face >= 3);

                *(casted_ptr + elem_offset) = num_vertices_around_face;
                ++elem_offset;
            }
#endif
        }
    } break;
    case MC_CONNECTED_COMPONENT_DATA_FACE_ADJACENT_FACE: {
        SCOPED_TIMER("MC_CONNECTED_COMPONENT_DATA_FACE_ADJACENT_FACE");
        if (pMem == nullptr) {

            MCUT_ASSERT(pNumBytes != nullptr);

            uint32_t num_face_adjacent_face_indices = 0;

#if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
            {
                // each worker-thread will count the number of indices according
                // to the number of faces in its range/block. The master thread
                // will then sum the total from all threads

                auto fn_count_faces_around_face = [&cc_uptr](face_array_iterator_t block_start_, face_array_iterator_t block_end_) {
                    uint32_t num_face_adjacent_face_indices_LOCAL = 0;

                    for (face_array_iterator_t fiter = block_start_; fiter != block_end_; ++fiter) {

                        const uint32_t num_faces_around_face = cc_uptr->kernel_hmesh_data->mesh->get_num_faces_around_face(*fiter, nullptr);
                        num_face_adjacent_face_indices_LOCAL += num_faces_around_face;
                    }

                    return num_face_adjacent_face_indices_LOCAL;
                };

                std::vector<std::future<uint32_t>> futures;
                uint32_t partial_res;

                parallel_for(
                    context_ptr->get_shared_compute_threadpool(),
                    cc_uptr->kernel_hmesh_data->mesh->faces_begin(),
                    cc_uptr->kernel_hmesh_data->mesh->faces_end(),
                    fn_count_faces_around_face,
                    partial_res, // output computed by master thread
                    futures);

                const uint32_t& num_face_adjacent_face_indices_MASTER_THREAD_LOCAL = partial_res;
                num_face_adjacent_face_indices += num_face_adjacent_face_indices_MASTER_THREAD_LOCAL;

                // wait for all worker-threads to finish copies
                for (uint32_t i = 0; i < (uint32_t)futures.size(); ++i) {
                    const uint32_t num_face_adjacent_face_indices_THREAD_LOCAL = futures[i].get();
                    num_face_adjacent_face_indices += num_face_adjacent_face_indices_THREAD_LOCAL;
                }
            }
#else // #if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
            for (face_array_iterator_t fiter = cc_uptr->kernel_hmesh_data->mesh->faces_begin(); fiter != cc_uptr->kernel_hmesh_data->mesh->faces_end(); ++fiter) {
                const uint32_t num_faces_around_face = cc_uptr->kernel_hmesh_data->mesh->get_num_faces_around_face(*fiter, nullptr);
                num_face_adjacent_face_indices += num_faces_around_face;
            }
#endif

            *pNumBytes = num_face_adjacent_face_indices * sizeof(uint32_t);
        } else {

            if (bytes % sizeof(uint32_t) != 0) {
                throw std::invalid_argument("invalid number of bytes");
            }

            uint32_t* casted_ptr = reinterpret_cast<uint32_t*>(pMem);

#if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
            {
                // step 1: compute array storing face-adjacent-face sizes (recursive API call)
                // - for computing exclusive sum
                // step 2: compute exclusive sum array ( in spirit of std::exclusive_scan)
                // - for determining per-thread (work-block) output-array offsets
                // step 3: copy adjacent face indices into output array using offsets from previous steps
                // - final result that is stored in user-output array

                const uint32_t nfaces = cc_uptr->kernel_hmesh_data->mesh->number_of_faces();

                //
                // step 1
                //

                // If client already called mcGetConnectedComponentData(..., MC_CONNECTED_COMPONENT_DATA_FACE_ADJACENT_FACE_SIZE, numBytes, faceAdjFaceSizes.data(), NULL)
                // then we should have already cached the array of face sizes

                if (!cc_uptr->face_adjacent_faces_size_cache_initialized) { // fill the cache by calling the API, within the API!

                    // this is like resizing output array in the client application, after knowing the number of faces in CC
                    cc_uptr->face_adjacent_faces_size_cache.resize(nfaces);

                    const std::size_t num_bytes = nfaces * sizeof(uint32_t);

                    // recursive Internal API call: populate cache here, which also sets "cc_uptr->face_adjacent_faces_size_cache" to true
                    get_connected_component_data_impl_detail(
                        context_ptr,
                        connCompId,
                        MC_CONNECTED_COMPONENT_DATA_FACE_ADJACENT_FACE_SIZE,
                        num_bytes,
                        cc_uptr->face_adjacent_faces_size_cache.data(),
                        NULL);
                }

                MCUT_ASSERT(cc_uptr->face_adjacent_faces_size_cache.empty() == false);
                MCUT_ASSERT(cc_uptr->face_adjacent_faces_size_cache_initialized == true);

                //
                // step 2
                //
                std::vector<uint32_t> partial_sum_vec = cc_uptr->face_adjacent_faces_size_cache; // copy

                parallel_partial_sum(context_ptr->get_shared_compute_threadpool(), partial_sum_vec.begin(), partial_sum_vec.end());

                //
                // step 3
                //

                auto fn_face_adjface_indices_copy = [&cc_uptr, &partial_sum_vec, &casted_ptr](face_array_iterator_t block_start_, face_array_iterator_t block_end_) {
                    const uint32_t base_face_offset = std::distance(cc_uptr->kernel_hmesh_data->mesh->faces_begin(), block_start_);

                    MCUT_ASSERT(base_face_offset < (uint32_t)cc_uptr->face_adjacent_faces_size_cache.size());

                    // the first face in the range between block start and end
                    const uint32_t base_face_adjface_count = SAFE_ACCESS(cc_uptr->face_adjacent_faces_size_cache, base_face_offset);

                    const uint32_t partial_sum_vec_val = SAFE_ACCESS(partial_sum_vec, base_face_offset);
                    const uint32_t index_arr_base_offset = partial_sum_vec_val - base_face_adjface_count;
                    uint32_t index_arr_offset = index_arr_base_offset;

                    std::vector<fd_t> faces_around_face; // tmp to prevent reallocations
                    faces_around_face.reserve(3);

                    for (face_array_iterator_t f_iter = block_start_; f_iter != block_end_; ++f_iter) {

                        faces_around_face.clear();
                        cc_uptr->kernel_hmesh_data->mesh->get_faces_around_face(faces_around_face, *f_iter);
                        const uint32_t num_faces_around_face = (uint32_t)faces_around_face.size();

                        // for each vertex in face
                        for (uint32_t i = 0; i < num_faces_around_face; ++i) {
                            const uint32_t face_idx = (uint32_t)SAFE_ACCESS(faces_around_face, i);
                            *(casted_ptr + index_arr_offset) = face_idx;
                            ++index_arr_offset;
                        }
                    }
                };

                parallel_for(
                    context_ptr->get_shared_compute_threadpool(),
                    cc_uptr->kernel_hmesh_data->mesh->faces_begin(),
                    cc_uptr->kernel_hmesh_data->mesh->faces_end(),
                    fn_face_adjface_indices_copy); // blocks until all work is done
            }
#else // #if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
            McSize elem_offset = 0;
            std::vector<fd_t> faces_around_face;

            for (face_array_iterator_t fiter = cc_uptr->kernel_hmesh_data->mesh->faces_begin(); fiter != cc_uptr->kernel_hmesh_data->mesh->faces_end(); ++fiter) {

                faces_around_face.clear();
                cc_uptr->kernel_hmesh_data->mesh->get_faces_around_face(faces_around_face, *fiter, nullptr);

                if (!faces_around_face.empty()) {
                    for (uint32_t i = 0; i < (uint32_t)faces_around_face.size(); ++i) {
                        *(casted_ptr + elem_offset) = (uint32_t)faces_around_face[i];
                        elem_offset++;
                    }
                }
            }

            MCUT_ASSERT((elem_offset * sizeof(uint32_t)) <= bytes);
#endif // #if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
        }
    } break;
    case MC_CONNECTED_COMPONENT_DATA_FACE_ADJACENT_FACE_SIZE: {
        SCOPED_TIMER("MC_CONNECTED_COMPONENT_DATA_FACE_ADJACENT_FACE_SIZE");

        if (pMem == nullptr) {
            *pNumBytes = cc_uptr->kernel_hmesh_data->mesh->number_of_faces() * sizeof(uint32_t); // each face has a size value (num adjacent faces)
        } else {
            if (bytes > cc_uptr->kernel_hmesh_data->mesh->number_of_faces() * sizeof(uint32_t)) {
                throw std::invalid_argument("out of bounds memory access");
            }

            if (bytes % sizeof(uint32_t) != 0) {
                throw std::invalid_argument("invalid number of bytes");
            }

            uint32_t* casted_ptr = reinterpret_cast<uint32_t*>(pMem);

#if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
            {
                const uint32_t face_count = cc_uptr->kernel_hmesh_data->mesh->number_of_faces();

                if (!cc_uptr->face_adjacent_faces_size_cache_initialized) { // init cache by storing data into it
                    // the code execution can also reach here because we asked for the cache
                    // to be populated in order to compute/copy the face indices in parallel.
                    // see: MC_CONNECTED_COMPONENT_DATA_FACE case
                    const bool cache_allocated_prior = !cc_uptr->face_adjacent_faces_size_cache.empty();

                    if (!cache_allocated_prior) {
                        cc_uptr->face_adjacent_faces_size_cache.resize(face_count);
                    }

                    auto fn_face_adj_face_size = [&cc_uptr](std::vector<uint32_t>::iterator block_start_, std::vector<uint32_t>::iterator block_end_) {
                        const uint32_t face_base_offset = (uint32_t)std::distance(cc_uptr->face_adjacent_faces_size_cache.begin(), block_start_);
                        uint32_t face_offset = face_base_offset;

                        for (std::vector<uint32_t>::iterator fs_iter = block_start_; fs_iter != block_end_; ++fs_iter) {

                            const face_descriptor_t descr(face_offset);

                            const uint32_t num_faces_around_face = cc_uptr->kernel_hmesh_data->mesh->get_num_faces_around_face(descr);

                            *fs_iter = num_faces_around_face;

                            face_offset++;
                        }
                    };

                    parallel_for(
                        context_ptr->get_shared_compute_threadpool(),
                        cc_uptr->face_adjacent_faces_size_cache.begin(),
                        cc_uptr->face_adjacent_faces_size_cache.end(),
                        fn_face_adj_face_size); // blocks until all work is done

                    cc_uptr->face_adjacent_faces_size_cache_initialized = true;
                }

                // the pointers are different if "cc_uptr->face_adjacent_faces_size_cache" is not
                // being populated in the current call
                const McVoid* src_ptr = reinterpret_cast<McVoid*>(&(cc_uptr->face_adjacent_faces_size_cache[0]));
                const McVoid* dst_ptr = pMem;
                const bool writing_to_client_pointer = (src_ptr != dst_ptr);

                if (writing_to_client_pointer) // copy only if "casted_ptr" is client pointer
                {
                    // copy to user pointer
                    memcpy(casted_ptr, &(cc_uptr->face_adjacent_faces_size_cache[0]), sizeof(uint32_t) * face_count);
                }
            }
#else // #if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
            McSize elem_offset = 0;
            for (face_array_iterator_t fiter = cc_uptr->kernel_hmesh_data->mesh->faces_begin(); fiter != cc_uptr->kernel_hmesh_data->mesh->faces_end(); ++fiter) {
                const uint32_t num_faces_around_face = cc_uptr->kernel_hmesh_data->mesh->get_num_faces_around_face(*fiter, nullptr);
                *(casted_ptr + elem_offset) = num_faces_around_face;
                elem_offset++;
            }

            MCUT_ASSERT((elem_offset * sizeof(uint32_t)) <= bytes);
#endif // #if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
        }
    } break;

    case MC_CONNECTED_COMPONENT_DATA_EDGE: {
        if (pMem == nullptr) {
            *pNumBytes = cc_uptr->kernel_hmesh_data->mesh->number_of_edges() * 2 * sizeof(uint32_t); // each edge has two indices
        } else {
            if (bytes > cc_uptr->kernel_hmesh_data->mesh->number_of_edges() * 2 * sizeof(uint32_t)) {
                throw std::invalid_argument("out of bounds memory access");
            }

            if (bytes % (sizeof(uint32_t) * 2) != 0) {
                throw std::invalid_argument("invalid number of bytes");
            }

            uint32_t* casted_ptr = reinterpret_cast<uint32_t*>(pMem);

#if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
            {
                auto fn_copy_edges = [&casted_ptr, &cc_uptr](edge_array_iterator_t block_start_, edge_array_iterator_t block_end_) {
                    // thread starting offset (in edge count) in the "array of edges"
                    const McSize base_offset = std::distance(cc_uptr->kernel_hmesh_data->mesh->edges_begin(), block_start_);

                    McSize elem_offset = base_offset * 2; // two (vertex) indices per edge

                    for (edge_array_iterator_t edge_iter = block_start_; edge_iter != block_end_; ++edge_iter) {

                        const vertex_descriptor_t v0 = cc_uptr->kernel_hmesh_data->mesh->vertex(*edge_iter, 0);
                        *(casted_ptr + elem_offset) = (uint32_t)v0;
                        elem_offset++;

                        const vertex_descriptor_t v1 = cc_uptr->kernel_hmesh_data->mesh->vertex(*edge_iter, 1);
                        *(casted_ptr + elem_offset) = (uint32_t)v1;
                        elem_offset++;
                    }
                };

                parallel_for(
                    context_ptr->get_shared_compute_threadpool(),
                    cc_uptr->kernel_hmesh_data->mesh->edges_begin(),
                    cc_uptr->kernel_hmesh_data->mesh->edges_end(),
                    fn_copy_edges);
            }
#else // #if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
            McSize elem_offset = 0;
            for (edge_array_iterator_t eiter = cc_uptr->kernel_hmesh_data->mesh->edges_begin(); eiter != cc_uptr->kernel_hmesh_data->mesh->edges_end(); ++eiter) {
                const vertex_descriptor_t v0 = cc_uptr->kernel_hmesh_data->mesh->vertex(*eiter, 0);
                *(casted_ptr + elem_offset) = (uint32_t)v0;
                elem_offset++;

                const vertex_descriptor_t v1 = cc_uptr->kernel_hmesh_data->mesh->vertex(*eiter, 1);
                *(casted_ptr + elem_offset) = (uint32_t)v1;
                elem_offset++;
            }

            MCUT_ASSERT((elem_offset * sizeof(uint32_t)) <= bytes);
#endif // #if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
        }
    } break;
    case MC_CONNECTED_COMPONENT_DATA_TYPE: {
        if (pMem == nullptr) {
            *pNumBytes = sizeof(McConnectedComponentType);
        } else {
            if (bytes > sizeof(McConnectedComponentType)) {
                throw std::invalid_argument("out of bounds memory access");
            }
            if (bytes % sizeof(McConnectedComponentType) != 0) {
                throw std::invalid_argument("invalid number of bytes");
            }
            memcpy(pMem, reinterpret_cast<McVoid*>(&cc_uptr->type), bytes);
        }
    } break;
    case MC_CONNECTED_COMPONENT_DATA_FRAGMENT_LOCATION: {

        if (cc_uptr->type != MC_CONNECTED_COMPONENT_TYPE_FRAGMENT) {
            throw std::invalid_argument("invalid client pointer type");
        }

        if (pMem == nullptr) {
            *pNumBytes = sizeof(McFragmentLocation);
        } else {

            if (bytes > sizeof(McFragmentLocation)) {
                throw std::invalid_argument("out of bounds memory access");
            }

            if (bytes % sizeof(McFragmentLocation) != 0) {
                throw std::invalid_argument("invalid number of bytes");
            }

            fragment_cc_t* fragPtr = dynamic_cast<fragment_cc_t*>(cc_uptr.get());
            memcpy(pMem, reinterpret_cast<McVoid*>(&fragPtr->fragmentLocation), bytes);
        }
    } break;
    case MC_CONNECTED_COMPONENT_DATA_PATCH_LOCATION: {

        if (cc_uptr->type != MC_CONNECTED_COMPONENT_TYPE_FRAGMENT && cc_uptr->type != MC_CONNECTED_COMPONENT_TYPE_PATCH) {
            throw std::invalid_argument("connected component must be a patch or a fragment");
        }

        if (pMem == nullptr) {
            *pNumBytes = sizeof(McPatchLocation);
        } else {
            if (bytes > sizeof(McPatchLocation)) {
                throw std::invalid_argument("out of bounds memory access");
            }

            if (bytes % sizeof(McPatchLocation) != 0) {
                throw std::invalid_argument("invalid number of bytes");
            }

            const McVoid* src = nullptr;
            if (cc_uptr->type == MC_CONNECTED_COMPONENT_TYPE_FRAGMENT) {
                src = reinterpret_cast<const McVoid*>(&dynamic_cast<fragment_cc_t*>(cc_uptr.get())->patchLocation);
            } else {
                MCUT_ASSERT(cc_uptr->type == MC_CONNECTED_COMPONENT_TYPE_PATCH);
                src = reinterpret_cast<const McVoid*>(&dynamic_cast<patch_cc_t*>(cc_uptr.get())->patchLocation);
            }
            memcpy(pMem, src, bytes);
        }
    } break;
    case MC_CONNECTED_COMPONENT_DATA_FRAGMENT_SEAL_TYPE: {

        if (cc_uptr->type != MC_CONNECTED_COMPONENT_TYPE_FRAGMENT) {
            throw std::invalid_argument("invalid client pointer type");
        }

        if (pMem == nullptr) {
            *pNumBytes = sizeof(McFragmentSealType);
        } else {
            if (bytes > sizeof(McFragmentSealType)) {
                throw std::invalid_argument("out of bounds memory access");
            }

            if (bytes % sizeof(McFragmentSealType) != 0) {
                throw std::invalid_argument("invalid number of bytes");
            }
            fragment_cc_t* fragPtr = dynamic_cast<fragment_cc_t*>(cc_uptr.get());
            memcpy(pMem, reinterpret_cast<McVoid*>(&fragPtr->srcMeshSealType), bytes);
        }
    } break;
        //
    case MC_CONNECTED_COMPONENT_DATA_ORIGIN: {

        if (cc_uptr->type != MC_CONNECTED_COMPONENT_TYPE_SEAM && cc_uptr->type != MC_CONNECTED_COMPONENT_TYPE_INPUT) {
            throw std::invalid_argument("invalid connected component type");
        }

        size_t nbytes = (cc_uptr->type != MC_CONNECTED_COMPONENT_TYPE_SEAM ? sizeof(McSeamOrigin) : sizeof(McInputOrigin));

        if (pMem == nullptr) {
            *pNumBytes = nbytes;
        } else {
            if (bytes > nbytes) {
                throw std::invalid_argument("out of bounds memory access");
            }

            if ((bytes % nbytes) != 0) {
                throw std::invalid_argument("invalid number of bytes");
            }

            if (cc_uptr->type == MC_CONNECTED_COMPONENT_TYPE_SEAM) {
                seam_cc_t* ptr = dynamic_cast<seam_cc_t*>(cc_uptr.get());
                memcpy(pMem, reinterpret_cast<McVoid*>(&ptr->origin), bytes);
            } else {
                input_cc_t* ptr = dynamic_cast<input_cc_t*>(cc_uptr.get());
                memcpy(pMem, reinterpret_cast<McVoid*>(&ptr->origin), bytes);
            }
        }
    } break;
    case MC_CONNECTED_COMPONENT_DATA_SEAM_VERTEX: {
        if (cc_uptr->type == MC_CONNECTED_COMPONENT_TYPE_INPUT) {
            throw std::invalid_argument("cannot query seam vertices on input connected component");
        }

        const uint32_t seam_vertex_count = (uint32_t)cc_uptr->kernel_hmesh_data->seam_vertices.size();

        if (pMem == nullptr) {
            *pNumBytes = seam_vertex_count * sizeof(uint32_t);
        } else {
            if (bytes > (seam_vertex_count * sizeof(uint32_t))) {
                throw std::invalid_argument("out of bounds memory access");
            }

            if ((bytes % (sizeof(uint32_t))) != 0) {
                throw std::invalid_argument("invalid number of bytes");
            }

            const uint32_t elems_to_copy = bytes / sizeof(uint32_t);

            uint32_t* casted_ptr = reinterpret_cast<uint32_t*>(pMem);

#if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
            {
                auto fn_copy_seam_vertices = [&casted_ptr, &cc_uptr, &elems_to_copy](std::vector<vd_t>::const_iterator block_start_, std::vector<vd_t>::const_iterator block_end_) {
                    // thread starting offset (in edge count) in the "array of edges"
                    const McSize base_offset = std::distance(cc_uptr->kernel_hmesh_data->seam_vertices.cbegin(), block_start_);

                    McSize elem_offset = base_offset;

                    for (std::vector<vd_t>::const_iterator sv_iter = block_start_; sv_iter != block_end_; ++sv_iter) {

                        *(casted_ptr + elem_offset) = (uint32_t)(*sv_iter);
                        elem_offset++;

                        if (elem_offset == elems_to_copy) {
                            break;
                        }
                    }
                };

                parallel_for(
                    context_ptr->get_shared_compute_threadpool(),
                    cc_uptr->kernel_hmesh_data->seam_vertices.cbegin(),
                    cc_uptr->kernel_hmesh_data->seam_vertices.cend(),
                    fn_copy_seam_vertices);
            }
#else // #if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
            uint32_t elem_offset = 0;
            for (uint32_t i = 0; i < elems_to_copy; ++i) {
                const uint32_t seam_vertex_idx = cc_uptr->kernel_hmesh_data->seam_vertices[i];
                *(casted_ptr + elem_offset) = seam_vertex_idx;
                elem_offset++;
            }

            MCUT_ASSERT(elem_offset <= seam_vertex_count);
#endif
        }
    } break;
    case MC_CONNECTED_COMPONENT_DATA_VERTEX_MAP: {
        SCOPED_TIMER("MC_CONNECTED_COMPONENT_DATA_VERTEX_MAP");

        const uint32_t vertex_map_size = cc_uptr->kernel_hmesh_data->data_maps.vertex_map.size();

        if (vertex_map_size == 0) {
            throw std::invalid_argument("vertex map not available"); // user probably forgot to set the dispatch flag
        }

        MCUT_ASSERT(vertex_map_size == (uint32_t)cc_uptr->kernel_hmesh_data->mesh->number_of_vertices());

        if (pMem == nullptr) {
            *pNumBytes = (vertex_map_size * sizeof(uint32_t)); // each each vertex has a map value (intersection point == uint_max)
        } else {
            if (bytes > (vertex_map_size * sizeof(uint32_t))) {
                throw std::invalid_argument("out of bounds memory access");
            }

            if (bytes % (sizeof(uint32_t)) != 0) {
                throw std::invalid_argument("invalid number of bytes");
            }

            const uint32_t elems_to_copy = (bytes / sizeof(uint32_t));

            MCUT_ASSERT(elems_to_copy <= vertex_map_size);

            uint32_t* casted_ptr = reinterpret_cast<uint32_t*>(pMem);

#if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
            {
                auto fn_copy_vertex_map = [&casted_ptr, &cc_uptr, &elems_to_copy](std::vector<vd_t>::const_iterator block_start_, std::vector<vd_t>::const_iterator block_end_) {
                    // thread starting offset
                    const uint32_t base_offset = (uint32_t)std::distance(cc_uptr->kernel_hmesh_data->data_maps.vertex_map.cbegin(), block_start_);

                    uint32_t elem_offset = base_offset;

                    for (std::vector<vd_t>::const_iterator v_iter = block_start_; v_iter != block_end_; ++v_iter) {

                        if ((elem_offset) >= elems_to_copy) {
                            break;
                        }

                        uint32_t i = elem_offset;

                        // Refer to single-threaded code (below) for documentation
                        uint32_t internal_input_mesh_vertex_idx = cc_uptr->kernel_hmesh_data->data_maps.vertex_map[i];
                        uint32_t client_input_mesh_vertex_idx = UINT32_MAX;
                        const bool internal_input_mesh_vertex_is_intersection_point = (internal_input_mesh_vertex_idx == UINT32_MAX);

                        if (!internal_input_mesh_vertex_is_intersection_point) {

                            bool vertex_exists_due_to_face_partitioning = false;
                            const bool internal_input_mesh_vertex_is_for_source_mesh = (internal_input_mesh_vertex_idx < cc_uptr->internal_sourcemesh_vertex_count);

                            if (internal_input_mesh_vertex_is_for_source_mesh) {
                                const std::unordered_map<vd_t, vec3>::const_iterator fiter = cc_uptr->source_hmesh_new_poly_partition_vertices->find(vd_t(internal_input_mesh_vertex_idx));
                                vertex_exists_due_to_face_partitioning = (fiter != cc_uptr->source_hmesh_new_poly_partition_vertices->cend());
                            } else {
                                std::unordered_map<vd_t, vec3>::const_iterator fiter = cc_uptr->cut_hmesh_new_poly_partition_vertices->find(vd_t(internal_input_mesh_vertex_idx));
                                vertex_exists_due_to_face_partitioning = (fiter != cc_uptr->cut_hmesh_new_poly_partition_vertices->cend());
                            }

                            if (!vertex_exists_due_to_face_partitioning) {

                                MCUT_ASSERT(cc_uptr->internal_sourcemesh_vertex_count > 0);

                                if (!internal_input_mesh_vertex_is_for_source_mesh) {
                                    const uint32_t internal_input_mesh_vertex_idx_without_offset = (internal_input_mesh_vertex_idx - cc_uptr->internal_sourcemesh_vertex_count);
                                    client_input_mesh_vertex_idx = (internal_input_mesh_vertex_idx_without_offset + cc_uptr->client_sourcemesh_vertex_count); // ensure that we offset using number of [user-provided mesh] vertices
                                } else {
                                    client_input_mesh_vertex_idx = internal_input_mesh_vertex_idx;
                                }
                            }
                        }

                        *(casted_ptr + elem_offset) = client_input_mesh_vertex_idx;
                        elem_offset++;
                    }
                };

                parallel_for(
                    context_ptr->get_shared_compute_threadpool(),
                    cc_uptr->kernel_hmesh_data->data_maps.vertex_map.cbegin(),
                    cc_uptr->kernel_hmesh_data->data_maps.vertex_map.cend(),
                    fn_copy_vertex_map);
            }
#else // #if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
            uint32_t elem_offset = 0;
            for (uint32_t i = 0; i < elems_to_copy; ++i) // ... for each vertex in CC
            {
                // Here we use whatever index value was assigned to the current vertex by the kernel, where the
                // the kernel does not necessarilly know that the input meshes it was given where modified by
                // the frontend (in this case via polygon partitioning)
                // Vertices that are polygon intersection points have a value of uint_max i.e. null_vertex().

                uint32_t internal_input_mesh_vertex_idx = cc_uptr->kernel_hmesh_data->data_maps.vertex_map[i];
                // We use the same default value as that used by the kernel for intersection
                // points (intersection points at mapped to uint_max i.e. null_vertex())
                uint32_t client_input_mesh_vertex_idx = UINT32_MAX;
                // This is true only for polygon intersection points computed by the kernel
                const bool internal_input_mesh_vertex_is_intersection_point = (internal_input_mesh_vertex_idx == UINT32_MAX);

                if (!internal_input_mesh_vertex_is_intersection_point) { // i.e. a client-mesh vertex or vertex that is added due to face-partitioning
                    // NOTE: The kernel will assign/map a 'proper' index value to vertices that exist due to face partitioning.
                    // 'proper' here means that the kernel treats these vertices as 'original vertices' from a client-provided input
                    // mesh. In reality, the frontend added such vertices in order to partition a face. i.e. the kernel is not aware
                    // that a given input mesh it is working with is modified by the frontend (it assumes that the meshes is exactly as was
                    // provided by the client).
                    // So, here we have to fix that mapping information to correctly state that "any vertex added due to face
                    // partitioning was not in the user provided input mesh" and should therefore be treated/labelled as an intersection
                    // point i.e. it should map to UINT32_MAX because it does not map to any vertex in the client-provided input mesh.
                    bool vertex_exists_due_to_face_partitioning = false;
                    // this flag tells us whether the current vertex maps to one in the internal version of the source mesh
                    // i.e. it does not map to the internal version cut-mesh
                    const bool internal_input_mesh_vertex_is_for_source_mesh = (internal_input_mesh_vertex_idx < cc_uptr->internal_sourcemesh_vertex_count);

                    if (internal_input_mesh_vertex_is_for_source_mesh) {
                        const std::unordered_map<vd_t, vec3>::const_iterator fiter = cc_uptr->source_hmesh_new_poly_partition_vertices->find(vd_t(internal_input_mesh_vertex_idx));
                        vertex_exists_due_to_face_partitioning = (fiter != cc_uptr->source_hmesh_new_poly_partition_vertices->cend());
                    } else // i.e. internal_input_mesh_vertex_is_for_cut_mesh
                    {
                        std::unordered_map<vd_t, vec3>::const_iterator fiter = cc_uptr->cut_hmesh_new_poly_partition_vertices->find(vd_t(internal_input_mesh_vertex_idx));
                        vertex_exists_due_to_face_partitioning = (fiter != cc_uptr->cut_hmesh_new_poly_partition_vertices->cend());
                    }

                    if (!vertex_exists_due_to_face_partitioning) { // i.e. is a client-mesh vertex (an original vertex)

                        MCUT_ASSERT(cc_uptr->internal_sourcemesh_vertex_count > 0);

                        if (!internal_input_mesh_vertex_is_for_source_mesh) // is it a cut-mesh vertex discriptor ..?
                        {
                            // vertices added due to face-partitioning will have an offsetted index/descriptor that is >= client_sourcemesh_vertex_count
                            const uint32_t internal_input_mesh_vertex_idx_without_offset = (internal_input_mesh_vertex_idx - cc_uptr->internal_sourcemesh_vertex_count);
                            client_input_mesh_vertex_idx = (internal_input_mesh_vertex_idx_without_offset + cc_uptr->client_sourcemesh_vertex_count); // ensure that we offset using number of [user-provided mesh] vertices
                        } else {
                            client_input_mesh_vertex_idx = internal_input_mesh_vertex_idx; // src-mesh vertices have no offset unlike cut-mesh vertices
                        }
                    }
                }

                *(casted_ptr + elem_offset) = client_input_mesh_vertex_idx;
                elem_offset++;
            }

            MCUT_ASSERT(elem_offset <= vertex_map_size);
#endif // #if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
        }
    } break;
    case MC_CONNECTED_COMPONENT_DATA_FACE_MAP: {
        SCOPED_TIMER("MC_CONNECTED_COMPONENT_DATA_FACE_MAP");

        const uint32_t face_map_size = cc_uptr->kernel_hmesh_data->data_maps.face_map.size();

        if (face_map_size == 0) {
            throw std::invalid_argument("face map not available"); // user probably forgot to set the dispatch flag
        }

        MCUT_ASSERT(face_map_size == (uint32_t)cc_uptr->kernel_hmesh_data->mesh->number_of_faces());

        if (pMem == nullptr) {
            *pNumBytes = face_map_size * sizeof(uint32_t); // each face has a map value (intersection point == uint_max)
        } else {
            if (bytes > (face_map_size * sizeof(uint32_t))) {
                throw std::invalid_argument("out of bounds memory access");
            }

            if ((bytes % sizeof(uint32_t)) != 0) {
                throw std::invalid_argument("invalid number of bytes");
            }

            const uint32_t elems_to_copy = (bytes / sizeof(uint32_t));

            uint32_t* casted_ptr = reinterpret_cast<uint32_t*>(pMem);

#if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
            {
                auto fn_copy_face_map = [&casted_ptr, &cc_uptr, &elems_to_copy](std::vector<fd_t>::const_iterator block_start_, std::vector<fd_t>::const_iterator block_end_) {
                    // thread starting offset (in edge count) in the "array of edges"
                    // thread starting offset
                    const uint32_t base_offset = (uint32_t)std::distance(cc_uptr->kernel_hmesh_data->data_maps.face_map.cbegin(), block_start_);

                    uint32_t elem_offset = base_offset;

                    for (std::vector<fd_t>::const_iterator f_iter = block_start_; f_iter != block_end_; ++f_iter) {

                        if ((elem_offset + 1) >= elems_to_copy) {
                            break;
                        }

                        uint32_t i = elem_offset;

                        // Refer to single-threaded code (below) for documentation
                        const uint32_t internal_inputmesh_face_idx = (uint32_t)cc_uptr->kernel_hmesh_data->data_maps.face_map[i];
                        // const uint32_t internal_inputmesh_face_idx = (uint32_t)cc_uptr->kernel_hmesh_data->data_maps.face_map[(uint32_t)*cc_face_iter];
                        const uint32_t user_inputmesh_face_idx = map_internal_inputmesh_face_idx_to_user_inputmesh_face_idx(
                            internal_inputmesh_face_idx,
                            cc_uptr);

                        *(casted_ptr + elem_offset) = user_inputmesh_face_idx;
                        elem_offset++;
                    }
                };

                parallel_for(
                    context_ptr->get_shared_compute_threadpool(),
                    cc_uptr->kernel_hmesh_data->data_maps.face_map.cbegin(),
                    cc_uptr->kernel_hmesh_data->data_maps.face_map.cend(),
                    fn_copy_face_map);
            }
#else // #if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
            uint32_t elem_offset = 0;
            for (uint32_t i = 0; i < elems_to_copy; ++i) // ... for each FACE (to copy) in CC
            {
                const uint32_t internal_inputmesh_face_idx = (uint32_t)cc_uptr->kernel_hmesh_data->data_maps.face_map[i];
                const uint32_t user_inputmesh_face_idx = map_internal_inputmesh_face_idx_to_user_inputmesh_face_idx(
                    internal_inputmesh_face_idx,
                    cc_uptr);

                *(casted_ptr + elem_offset) = user_inputmesh_face_idx;
                elem_offset++;
            }

            MCUT_ASSERT(elem_offset <= face_map_size);
#endif // #if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
        }
    } break;
    case MC_CONNECTED_COMPONENT_DATA_FACE_TRIANGULATION: {
        SCOPED_TIMER("MC_CONNECTED_COMPONENT_DATA_FACE_TRIANGULATION");
        // internal halfedge data structure from the current connected component
        const std::shared_ptr<hmesh_t> cc = cc_uptr->kernel_hmesh_data->mesh;

        const uint32_t nontri_face_map_size = cc_uptr->kernel_hmesh_data->data_maps.face_map.size();

        // user has set the dispatch flag to allow us to save the face maps.
        const bool user_requested_cdt_face_maps = (nontri_face_map_size != 0);

        if (cc_uptr->cdt_index_cache_initialized == false) // compute triangulation if not yet available
        {
            MCUT_ASSERT(cc_uptr->cdt_index_cache.empty());

            const uint32_t cc_face_count = cc->number_of_faces();
            if (user_requested_cdt_face_maps) {
                cc_uptr->cdt_face_map_cache.reserve(cc_face_count * 1.2);
            }

#if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
            {

                // the offset from which each thread will write its CDT indices
                // into "cc_uptr->cdt_index_cache"
                std::atomic<std::uint32_t> cdt_index_cache_offset;
                cdt_index_cache_offset.store(0);

                // The following scheduling parameters are needed because we need
                // to know how many threads will be schedule to perform triangulation.
                // (for the barrier primitive).
                // The _exact_ same information must be computed inside "parallel_for",
                // which is why "min_per_thread" needs to be explicitly passed to
                // "parallel_for" after setting it here.

                // number of threads to perform task (some/all of the pool threads plus master thread)
                uint32_t num_threads = 0;
                const uint32_t min_per_thread = 1 << 10;
                {
                    uint32_t max_threads = 0;
                    const uint32_t available_threads = context_ptr->get_shared_compute_threadpool().get_num_threads() + 1; // workers and master (+1)
                    uint32_t block_size_unused = 0;
                    const uint32_t length = cc_face_count;

                    get_scheduling_parameters(
                        num_threads,
                        max_threads,
                        block_size_unused,
                        length,
                        available_threads,
                        min_per_thread);
                }

                const std::thread::id master_thread_id = std::this_thread::get_id();

                barrier_t barrier(num_threads);

                auto fn_triangulate_faces = [&](face_array_iterator_t block_start_, face_array_iterator_t block_end_) {
                    // CDT indices computed per thread
                    std::vector<uint32_t> cdt_index_cache_local;
                    const uint32_t num_elems_to_process = (uint32_t)std::distance(block_start_, block_end_);
                    cdt_index_cache_local.reserve(num_elems_to_process * 4);
                    std::vector<uint32_t> cdt_face_map_cache_local;
                    cdt_face_map_cache_local.reserve(num_elems_to_process * 1.2);

                    std::vector<vertex_descriptor_t> cc_face_vertices;
                    std::vector<uint32_t> cc_face_triangulation;

                    for (face_array_iterator_t cc_face_iter = block_start_; cc_face_iter != block_end_; ++cc_face_iter) {

                        cc->get_vertices_around_face(cc_face_vertices, *cc_face_iter);

                        const uint32_t cc_face_vcount = (uint32_t)cc_face_vertices.size();

                        MCUT_ASSERT(cc_face_vcount >= 3);

                        const bool cc_face_is_triangle = (cc_face_vcount == 3);

                        if (cc_face_is_triangle) {

                            for (uint32_t i = 0; i < cc_face_vcount; ++i) {
                                const uint32_t vertex_id_in_cc = (uint32_t)SAFE_ACCESS(cc_face_vertices, i);
                                cdt_index_cache_local.push_back(vertex_id_in_cc);
                            }

                            if (user_requested_cdt_face_maps) {
                                const uint32_t internal_inputmesh_face_idx = (uint32_t)cc_uptr->kernel_hmesh_data->data_maps.face_map[(uint32_t)*cc_face_iter];
                                const uint32_t user_inputmesh_face_idx = map_internal_inputmesh_face_idx_to_user_inputmesh_face_idx(
                                    internal_inputmesh_face_idx,
                                    cc_uptr);
                                cdt_face_map_cache_local.push_back(user_inputmesh_face_idx);
                            }

                        } else {

                            cc_face_triangulation.clear();

                            triangulate_face(cc_face_triangulation, context_ptr, cc_face_vcount, cc_face_vertices, *(cc.get()), *cc_face_iter);

                            // NOTE: "cc_face_triangulation" can be empty if the face has near-zero area

                            for (uint32_t i = 0; i < (uint32_t)cc_face_triangulation.size(); ++i) {
                                const uint32_t local_idx = cc_face_triangulation[i]; // id local within the current face that we are triangulating
                                MCUT_ASSERT(local_idx < cc_face_vcount);
                                const uint32_t global_idx = (uint32_t)cc_face_vertices[local_idx];
                                MCUT_ASSERT(global_idx < (uint32_t)cc->number_of_vertices());

                                cdt_index_cache_local.push_back(global_idx);

                                if (user_requested_cdt_face_maps) {
                                    if ((i % 3) == 0) { // every three indices constitute one triangle
                                        // map every CDT triangle in "*cc_face_iter"  to the index value of "*cc_face_iter" (in the user input mesh)
                                        const uint32_t internal_inputmesh_face_idx = (uint32_t)cc_uptr->kernel_hmesh_data->data_maps.face_map[(uint32_t)*cc_face_iter];
                                        MCUT_ASSERT(internal_inputmesh_face_idx < cc_face_count);
                                        const uint32_t user_inputmesh_face_idx = map_internal_inputmesh_face_idx_to_user_inputmesh_face_idx(
                                            internal_inputmesh_face_idx,
                                            cc_uptr);
                                        MCUT_ASSERT(internal_inputmesh_face_idx < cc_face_count);
                                        cdt_face_map_cache_local.push_back(user_inputmesh_face_idx);
                                    }
                                }
                            }
                        }
                    }

                    // local num triangulation indices computed by thread
                    const uint32_t cdt_index_cache_local_len = cdt_index_cache_local.size();

                    const uint32_t write_offset = cdt_index_cache_offset.fetch_add(cdt_index_cache_local_len);

                    barrier.wait(); // .. for all threads to triangulate their range of faces

                    if (std::this_thread::get_id() == master_thread_id) {
                        //
                        // allocate memory for threads to write in parallel
                        //
                        const uint32_t num_triangulation_indices_global = cdt_index_cache_offset.load();
                        MCUT_ASSERT((num_triangulation_indices_global % 3) == 0);
                        const uint32_t num_triangles_global = num_triangulation_indices_global / 3;
                        MCUT_ASSERT(num_triangles_global >= 1);
                        cc_uptr->cdt_index_cache.resize(num_triangulation_indices_global);

                        if (user_requested_cdt_face_maps) {
                            cc_uptr->cdt_face_map_cache.resize(num_triangles_global);
                        }
                    }

                    barrier.wait(); // .. for memory to be allocated

                    // for each local triangulation index (NOTE: num triangles local = "cdt_index_cache_local_len/3")
                    for (uint32_t i = 0; i < cdt_index_cache_local_len; ++i) {

                        // local triangulation cache index
                        const uint32_t local_cache_idx = i;
                        // global triangulation cache index
                        const uint32_t global_cache_idx = write_offset + local_cache_idx;

                        SAFE_ACCESS(cc_uptr->cdt_index_cache, global_cache_idx) = SAFE_ACCESS(cdt_index_cache_local, local_cache_idx);

                        if (user_requested_cdt_face_maps && ((local_cache_idx % 3) == 0)) // every three cdt indices constitute a triangle
                        {
                            const uint32_t triangle_index_global = global_cache_idx / 3;
                            const uint32_t triangle_index_local = local_cache_idx / 3;

                            SAFE_ACCESS(cc_uptr->cdt_face_map_cache, triangle_index_global) = SAFE_ACCESS(cdt_face_map_cache_local, triangle_index_local);
                        }
                    }
                };

                parallel_for(
                    context_ptr->get_shared_compute_threadpool(),
                    cc_uptr->kernel_hmesh_data->mesh->faces_begin(),
                    cc_uptr->kernel_hmesh_data->mesh->faces_end(),
                    fn_triangulate_faces,
                    min_per_thread);
            }
#else // #if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
            uint32_t face_indices_offset = 0;
            cc_uptr->cdt_index_cache.reserve(cc_face_count * 1.2);

            // descriptors of vertices in face (they index into the CC)
            std::vector<vertex_descriptor_t> cc_face_vertices;

            for (face_array_iterator_t cc_face_iter = cc->faces_begin(); cc_face_iter != cc->faces_end(); ++cc_face_iter) {

                cc->get_vertices_around_face(cc_face_vertices, *cc_face_iter);

                // number of vertices of triangulated face
                const uint32_t cc_face_vcount = (uint32_t)cc_face_vertices.size();

                MCUT_ASSERT(cc_face_vcount >= 3);

                const bool cc_face_is_triangle = (cc_face_vcount == 3);

                if (cc_face_is_triangle) {

                    // for each vertex in face
                    for (uint32_t i = 0; i < cc_face_vcount; ++i) {
                        const uint32_t vertex_id_in_cc = (uint32_t)SAFE_ACCESS(cc_face_vertices, i);
                        cc_uptr->cdt_index_cache.push_back(vertex_id_in_cc);
                    }

                    if (user_requested_cdt_face_maps) {
                        const uint32_t internal_inputmesh_face_idx = (uint32_t)cc_uptr->kernel_hmesh_data->data_maps.face_map[(uint32_t)*cc_face_iter];
                        const uint32_t user_inputmesh_face_idx = map_internal_inputmesh_face_idx_to_user_inputmesh_face_idx(
                            internal_inputmesh_face_idx,
                            cc_uptr);
                        cc_uptr->cdt_face_map_cache.push_back(user_inputmesh_face_idx);
                    }

                } else {

                    //
                    // need to triangulate face
                    //

                    // List of indices which define all triangles that result from the CDT
                    // These indices are _local_ to the vertex list of the face being
                    // triangulated!
                    std::vector<uint32_t> cc_face_triangulation;

                    triangulate_face(cc_face_triangulation, context_ptr, cc_face_vcount, cc_face_vertices, cc.get()[0], *cc_face_iter);

                    if (cc_face_triangulation.empty() == false) {
                        //
                        // Change local triangle indices to global index values (in CC) and save
                        //

                        const uint32_t cc_face_triangulation_index_count = (uint32_t)cc_face_triangulation.size();
                        cc_uptr->cdt_index_cache.reserve(
                            cc_uptr->cdt_index_cache.size() + cc_face_triangulation_index_count);

                        for (uint32_t i = 0; i < cc_face_triangulation_index_count; ++i) {
                            const uint32_t local_idx = cc_face_triangulation[i]; // id local within the current face that we are triangulating
                            const uint32_t global_idx = (uint32_t)cc_face_vertices[local_idx];

                            cc_uptr->cdt_index_cache.push_back(global_idx);

                            if (user_requested_cdt_face_maps) {
                                if ((i % 3) == 0) { // every three indices constitute one triangle
                                    // map every CDT triangle in "*cc_face_iter"  to the index value of "*cc_face_iter" (in the user input mesh)
                                    const uint32_t internal_inputmesh_face_idx = (uint32_t)cc_uptr->kernel_hmesh_data->data_maps.face_map[(uint32_t)*cc_face_iter];
                                    const uint32_t user_inputmesh_face_idx = map_internal_inputmesh_face_idx_to_user_inputmesh_face_idx(
                                        internal_inputmesh_face_idx,
                                        cc_uptr);
                                    cc_uptr->cdt_face_map_cache.push_back(user_inputmesh_face_idx);
                                }
                            }
                        }
                    }
                } //  if (cc_face_vcount == 3)

                face_indices_offset += cc_face_vcount;
            }

            MCUT_ASSERT(cc_uptr->cdt_index_cache.size() >= 3);
#endif // #if defined(MCUT_WITH_COMPUTE_HELPER_THREADPOOL)
            cc_uptr->cdt_index_cache_initialized = true;

            if (user_requested_cdt_face_maps) {
                MCUT_ASSERT(cc_uptr->cdt_face_map_cache.empty() == false);
                cc_uptr->cdt_face_map_cache_initialized = true;
            }

        } // if(cc_uptr->indexArrayMesh.numTriangleIndices == 0)

        MCUT_ASSERT(cc_uptr->cdt_index_cache_initialized == true);

        if (user_requested_cdt_face_maps) {
            MCUT_ASSERT(!cc_uptr->cdt_face_map_cache.empty());
        }

        // i.e. pMem is a pointer allocated by the user and not one that we allocated
        // here inside the API e.g. fool us into just computing the CDT triangulation
        // indices and face map caches.
        // See also the case for MC_CONNECTED_COMPONENT_DATA_FACE_TRIANGULATION_MAP below:
        const bool proceed_and_copy_to_output_ptr = user_requested_cdt_face_maps == false || pMem != cc_uptr->cdt_face_map_cache.data();

        if (proceed_and_copy_to_output_ptr) {
            const uint32_t num_triangulation_indices = (uint32_t)cc_uptr->cdt_index_cache.size();

            if (pMem == nullptr) // client pointer is null (asking for size)
            {
                MCUT_ASSERT(num_triangulation_indices >= 3);
                *pNumBytes = num_triangulation_indices * sizeof(uint32_t); // each each vertex has a map value (intersection point == uint_max)
            } else {
                MCUT_ASSERT(num_triangulation_indices >= 3);

                if (bytes > num_triangulation_indices * sizeof(uint32_t)) {
                    throw std::invalid_argument("out of bounds memory access");
                }

                if (bytes % (sizeof(uint32_t)) != 0 || (bytes / sizeof(uint32_t)) % 3 != 0) {
                    throw std::invalid_argument("invalid number of bytes");
                }

                const bool flip_winding_order = context_ptr->get_connected_component_winding_order() == McConnectedComponentFaceWindingOrder::MC_CONNECTED_COMPONENT_FACE_WINDING_ORDER_REVERSED;

                if (flip_winding_order) {
                    const uint32_t n = (uint32_t)cc_uptr->cdt_index_cache.size();
                    for (uint32_t i = 0; i < n; ++i) {
                        ((uint32_t*)pMem)[n - 1 - i] = cc_uptr->cdt_index_cache[i];
                    }
                } else {
                    memcpy(pMem, reinterpret_cast<McVoid*>(cc_uptr->cdt_index_cache.data()), bytes);
                }
            }
        }
    } break;
    case MC_CONNECTED_COMPONENT_DATA_FACE_TRIANGULATION_MAP: {
        SCOPED_TIMER("MC_CONNECTED_COMPONENT_DATA_FACE_TRIANGULATION_MAP");
        // The default/standard non-tri face map. If this is defined then the tri-face map must also be defined
        const uint32_t face_map_size = cc_uptr->kernel_hmesh_data->data_maps.face_map.size();

        if (face_map_size == 0) {
            throw std::invalid_argument("face map not available"); // user probably forgot to set the dispatch flag
        } else {
            // Face maps are available (because they were requested by user) and
            // so it follows that the triangulated-face maps should be available too.
            MCUT_ASSERT(cc_uptr->cdt_face_map_cache_initialized == true);
        }

        MCUT_ASSERT(face_map_size == (uint32_t)cc_uptr->kernel_hmesh_data->mesh->number_of_faces());

        // Did the user request the triangulated-face map BEFORE the triangulated face indices?
        // That is call mcGetConnectedComponentData with MC_CONNECTED_COMPONENT_DATA_FACE_TRIANGULATION_MAP before calling with MC_CONNECTED_COMPONENT_DATA_FACE_TRIANGULATION
        // If so, we need to compute the triangulated face indices anyway (and cache then) since
        // that API call also compute the triangulated face maps (cache)
        if (cc_uptr->cdt_face_map_cache_initialized == false) {

            // recursive Internal API call to compute CDT and populate caches and also set "cc_uptr->cdt_face_map_cache_initialized" to true
            get_connected_component_data_impl_detail(
                context_ptr,
                connCompId,
                MC_CONNECTED_COMPONENT_DATA_FACE_TRIANGULATION,
                /*The next two parameters are actually unused (in the sense of writing data to them).
                They must be provided however, in order to fool the (internal) API call into deducing that we
                want to query triangulation data but what we really want to compute the CDT (cache) and
                populated the triangulated face maps (cache) */
                sizeof(uint32_t), // **
                cc_uptr->cdt_face_map_cache.data(), // value of pointer will also be used to infer that no memcpy is actually performed
                NULL);

            MCUT_ASSERT(cc_uptr->cdt_face_map_cache_initialized == true);
        }

        const uint32_t triangulated_face_map_size = cc_uptr->cdt_face_map_cache.size();

        MCUT_ASSERT(triangulated_face_map_size >= face_map_size);

        if (pMem == nullptr) {
            *pNumBytes = triangulated_face_map_size * sizeof(uint32_t); // each face has a map value (intersection point == uint_max)
        } else {
            if (bytes > (triangulated_face_map_size * sizeof(uint32_t))) {
                throw std::invalid_argument("out of bounds memory access");
            }

            if ((bytes % sizeof(uint32_t)) != 0) {
                throw std::invalid_argument("invalid number of bytes");
            }

            uint32_t* casted_ptr = reinterpret_cast<uint32_t*>(pMem);

            memcpy(casted_ptr, &cc_uptr->cdt_face_map_cache[0], bytes);
        }
    } break;
    default:
        throw std::invalid_argument("invalid enum flag");
    }
}

void get_connected_component_data_impl(
    const McContext contextHandle,
    const McConnectedComponent connCompId,
    McFlags flags,
    McSize bytes,
    McVoid* pMem,
    McSize* pNumBytes,
    uint32_t numEventsInWaitlist,
    const McEvent* pEventWaitList,
    McEvent* pEvent)
{
    std::shared_ptr<context_t> context_ptr = g_contexts.find_first_if([=](const std::shared_ptr<context_t> cptr) { return cptr->m_user_handle == contextHandle; });

    if (context_ptr == nullptr) {
        throw std::invalid_argument("invalid context");
    }

    std::weak_ptr<context_t> context_weak_ptr(context_ptr);

    const McEvent event_handle = context_ptr->prepare_and_submit_API_task(
        MC_COMMAND_GET_CONNECTED_COMPONENT_DATA, numEventsInWaitlist, pEventWaitList,
        [=]() {
            if (!context_weak_ptr.expired()) {
                std::shared_ptr<context_t> context = context_weak_ptr.lock();
                if (context) {
                    // asynchronously get the data and write to user provided pointer
                    get_connected_component_data_impl_detail(
                        context,
                        connCompId,
                        flags,
                        bytes,
                        pMem,
                        pNumBytes);
                }
            }
        });

    *pEvent = event_handle;
}

void release_event_impl(
    McEvent eventHandle)
{
    std::shared_ptr<event_t> event_ptr = g_events.find_first_if([=](const std::shared_ptr<event_t> eptr) { return eptr->m_user_handle == eventHandle; });

    if (event_ptr == nullptr) {
        throw std::invalid_argument("invalid event handle");
    }

    {
        g_events.remove_if([=](const std::shared_ptr<event_t> eptr) { return eptr->m_user_handle == eventHandle; });
    }
}

void release_events_impl(uint32_t numEvents, const McEvent* pEvents)
{
    for (uint32_t i = 0; i < numEvents; ++i) {
        McEvent eventHandle = pEvents[i];
        release_event_impl(eventHandle);
    }
}

void release_connected_components_impl(
    const McContext contextHandle,
    uint32_t numConnComps,
    const McConnectedComponent* pConnComps)
{
    std::shared_ptr<context_t> context_ptr = g_contexts.find_first_if([=](const std::shared_ptr<context_t> cptr) { return cptr->m_user_handle == contextHandle; });

    if (context_ptr == nullptr) {
        throw std::invalid_argument("invalid context");
    }

    bool freeAll = numConnComps == 0 && pConnComps == NULL;

    if (freeAll) {
        context_ptr->connected_components.remove_if([](const std::shared_ptr<connected_component_t>) { return true; });
    } else {
        for (int i = 0; i < (int)numConnComps; ++i) {
            McConnectedComponent connCompId = pConnComps[i];

            // report error if cc is not valid
            std::shared_ptr<connected_component_t> cc_ptr = context_ptr->connected_components.find_first_if([=](const std::shared_ptr<connected_component_t> ccptr) { return ccptr->m_user_handle == connCompId; });

            if (cc_ptr == nullptr) {
                throw std::invalid_argument("invalid connected component handle");
            }

            context_ptr->connected_components.remove_if([=](const std::shared_ptr<connected_component_t> ccptr) { return ccptr->m_user_handle == connCompId; });
        }
    }
}

void release_context_impl(
    McContext contextHandle)
{
    std::shared_ptr<context_t> context_ptr = g_contexts.find_first_if([=](const std::shared_ptr<context_t> cptr) { return cptr->m_user_handle == contextHandle; });

    if (context_ptr == nullptr) {
        throw std::invalid_argument("invalid context handle");
    }

    g_contexts.remove_if([=](const std::shared_ptr<context_t> cptr) { return cptr->m_user_handle == contextHandle; });
}