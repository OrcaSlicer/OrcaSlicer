#ifndef _BAMBU__TUNNEL_H_
#define _BAMBU__TUNNEL_H_

#ifdef BAMBU_DYNAMIC
#  define BAMBU_EXPORT
#  define BAMBU_FUNC(x) (*x)
#else
#  ifdef _WIN32
#    ifdef BAMBU_EXPORTS
#      define BAMBU_EXPORT __declspec(dllexport)
#    else
#      define BAMBU_EXPORT __declspec(dllimport)
#    endif // BAMBU_EXPORTS
#  else
#    define BAMBU_EXPORT
#  endif // __WIN32__
#  define BAMBU_FUNC(x) x
#endif // BAMBU_DYNAMIC

#ifdef __cplusplus
extern "C" {
#else
#include <stdbool.h>
#endif // __cplusplus

#ifdef _WIN32
#ifdef __cplusplus
    typedef wchar_t tchar;
#else
    typedef unsigned short tchar;
#endif
#else
    typedef char tchar;
#endif

typedef void* Bambu_Tunnel;

typedef void (*Logger)(void * context, int level, tchar const* msg);

typedef enum __Bambu_StreamType
{
    VIDE,
    AUDI
} Bambu_StreamType;

typedef enum __Bambu_VideoSubType
{
    AVC1,
    MJPG,
} Bambu_VideoSubType;

typedef enum __Bambu_AudioSubType
{
    MP4A
} Bambu_AudioSubType;

typedef enum __Bambu_FormatType
{
    video_avc_packet,
    video_avc_byte_stream,
    video_jpeg,
    audio_raw,
    audio_adts
} Bambu_FormatType;

typedef struct __Bambu_StreamInfo
{
    Bambu_StreamType type;
    int sub_type;
    union {
        struct
        {
            int width;
            int height;
            int frame_rate;
        } video;
        struct
        {
            int sample_rate;
            int channel_count;
            int sample_size;
        } audio;
    } format;
    int format_type;
    int format_size;
    int max_frame_size;
    unsigned char const * format_buffer;
} Bambu_StreamInfo;

typedef enum __Bambu_SampleFlag
{
    f_sync = 1
} Bambu_SampleFlag;

typedef struct __Bambu_Sample
{
    int itrack;
    int size;
    int flags;
    unsigned char const * buffer;
    unsigned long long decode_time;
} Bambu_Sample;

typedef enum __Bambu_Error
{
    Bambu_success,
    Bambu_stream_end,
    Bambu_would_block,
    Bambu_buffer_limit
} Bambu_Error;

#ifdef BAMBU_DYNAMIC
typedef struct __BambuLib {
#endif

BAMBU_EXPORT int BAMBU_FUNC(Bambu_Create)(Bambu_Tunnel* tunnel, char const* path);

BAMBU_EXPORT void BAMBU_FUNC(Bambu_SetLogger)(Bambu_Tunnel tunnel, Logger logger, void * context);

BAMBU_EXPORT int BAMBU_FUNC(Bambu_Open)(Bambu_Tunnel tunnel);

BAMBU_EXPORT int BAMBU_FUNC(Bambu_StartStream)(Bambu_Tunnel tunnel, bool video);

BAMBU_EXPORT int BAMBU_FUNC(Bambu_StartStreamEx)(Bambu_Tunnel tunnel, int type);

BAMBU_EXPORT int BAMBU_FUNC(Bambu_GetStreamCount)(Bambu_Tunnel tunnel);

BAMBU_EXPORT int BAMBU_FUNC(Bambu_GetStreamInfo)(Bambu_Tunnel tunnel, int index, Bambu_StreamInfo* info);

BAMBU_EXPORT unsigned long BAMBU_FUNC(Bambu_GetDuration)(Bambu_Tunnel tunnel);

BAMBU_EXPORT int BAMBU_FUNC(Bambu_Seek)(Bambu_Tunnel tunnel, unsigned long time);

BAMBU_EXPORT int BAMBU_FUNC(Bambu_ReadSample)(Bambu_Tunnel tunnel, Bambu_Sample* sample);

BAMBU_EXPORT int BAMBU_FUNC(Bambu_SendMessage)(Bambu_Tunnel tunnel, int ctrl, char const* data, int len);

BAMBU_EXPORT int BAMBU_FUNC(Bambu_RecvMessage)(Bambu_Tunnel tunnel, int* ctrl, char* data, int* len);

BAMBU_EXPORT void BAMBU_FUNC(Bambu_Close)(Bambu_Tunnel tunnel);

BAMBU_EXPORT void BAMBU_FUNC(Bambu_Destroy)(Bambu_Tunnel tunnel);

BAMBU_EXPORT int BAMBU_FUNC(Bambu_Init)();

BAMBU_EXPORT void BAMBU_FUNC(Bambu_Deinit)();

BAMBU_EXPORT char const* BAMBU_FUNC(Bambu_GetLastErrorMsg)();

BAMBU_EXPORT void BAMBU_FUNC(Bambu_FreeLogMsg)(tchar const* msg);

#ifdef BAMBU_DYNAMIC
} BambuLib;
#endif

#ifdef __cplusplus
}
#endif // __cplusplus

#endif // _BAMBU__TUNNEL_H_
