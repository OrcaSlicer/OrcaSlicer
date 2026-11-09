#include "slic3r/GUI/Printer/BambuTunnel.h"
#include <string>

namespace {
struct Camera {
    bool fail;
    unsigned frames = 0;
};
}

int Bambu_Create(Bambu_Tunnel* tunnel, const char* url)
{
    *tunnel = new Camera{std::string(url).find("failure.invalid") != std::string::npos};
    return 0;
}

void Bambu_SetLogger(Bambu_Tunnel, Logger logger, void* context)
{
#ifdef _WIN32
    logger(context, 1, L"Recoverable diagnostic [3]");
#else
    logger(context, 1, "Recoverable diagnostic [3]");
#endif
}

int Bambu_Open(Bambu_Tunnel) { return 0; }
int Bambu_StartStream(Bambu_Tunnel, bool) { return 0; }

int Bambu_ReadSample(Bambu_Tunnel tunnel, Bambu_Sample* sample)
{
    auto& camera = *static_cast<Camera*>(tunnel);
    if (camera.frames >= 2) return camera.fail ? Bambu_stream_end : Bambu_would_block;
    static const unsigned char bytes[] = {1, 2, 3};
    sample->buffer = bytes;
    sample->size = sizeof(bytes);
    sample->decode_time = ++camera.frames;
    return 0;
}

void Bambu_Close(Bambu_Tunnel) {}
void Bambu_Destroy(Bambu_Tunnel tunnel) { delete static_cast<Camera*>(tunnel); }
