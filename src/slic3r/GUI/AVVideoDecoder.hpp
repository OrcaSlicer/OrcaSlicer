#ifndef AVVIDEODECODER_HPP
#define AVVIDEODECODER_HPP

#include "Printer/BambuTunnel.h"

extern "C" {
    #include <libavcodec/avcodec.h>
    #include <libswscale/swscale.h>
}
#include <vector>
#include <wx/bitmap.h>
#include <wx/gdicmn.h>
#include <wx/image.h>

class wxBitmap;

class AVVideoDecoder
{
public:
    AVVideoDecoder();

    ~AVVideoDecoder();

public:
    int  open(Bambu_StreamInfo const &info);

    int  decode(Bambu_Sample const &sample);

    int  flush();

    void close();

    bool toWxImage(wxImage &image, wxSize const &size);

    bool toWxBitmap(wxBitmap &bitmap, wxSize const & size);

private:
    AVCodecContext *codec_ctx_ = nullptr;
    AVFrame *       frame_     = nullptr;
    SwsContext *    sws_ctx_   = nullptr;
    bool got_frame_ = false;
    int width_ { 0 }; // scale result width
    std::vector<uint8_t> bits_;
};

#endif // AVVIDEODECODER_HPP
