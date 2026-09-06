#include "AVVideoDecoder.hpp"

#include <assert.h>

extern "C"
{
    #include <libavutil/avutil.h>
    #include <libavutil/imgutils.h>
}

AVVideoDecoder::AVVideoDecoder()
{
    codec_ctx_ = avcodec_alloc_context3(nullptr);
}

AVVideoDecoder::~AVVideoDecoder()
{
    if (sws_ctx_)
        sws_freeContext(sws_ctx_);
    if (frame_)
        av_frame_free(&frame_);
    if (codec_ctx_)
        avcodec_free_context(&codec_ctx_);
}

int AVVideoDecoder::open(Bambu_StreamInfo const &info)
{
    auto codec_id = info.sub_type == AVC1 ? AV_CODEC_ID_H264 : AV_CODEC_ID_MJPEG;
    auto codec    = avcodec_find_decoder(codec_id);
    if (codec == nullptr) {
        fprintf(stderr, "AVVideoDecoder: unsupported codec!\n");
        return -1; // Codec not found
    }
    /* open the coderc */
    if (avcodec_open2(codec_ctx_, codec, nullptr) < 0) {
        fprintf(stderr, "AVVideoDecoder: could not open codec\n");
        return -1;
    }

    // Allocate an AVFrame structure
    frame_ = av_frame_alloc();
    if (frame_ == nullptr)
        return -1;

    return 0;
}

int AVVideoDecoder::decode(const Bambu_Sample &sample)
{
    int ret = -1;
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) {
        return ret;
    }

    ret = av_new_packet(pkt, sample.size);
    if (ret != 0) {
        av_packet_free(&pkt);
        return ret;
    }

    memcpy(pkt->data, sample.buffer, size_t(sample.size));

    ret = avcodec_send_packet(codec_ctx_, pkt);
    if (ret == 0) {
        got_frame_ = avcodec_receive_frame(codec_ctx_, frame_) == 0;
    }

    av_packet_unref(pkt);
    av_packet_free(&pkt);
    return ret;
}

int AVVideoDecoder::flush()
{
    int ret = avcodec_send_packet(codec_ctx_, nullptr);
    got_frame_ = avcodec_receive_frame(codec_ctx_, frame_) == 0;
    return ret;
}

void AVVideoDecoder::close()
{
}

bool AVVideoDecoder::toWxImage(wxImage &image, wxSize const &size2)
{
    if (!got_frame_)
        return false;

    auto size1 = size2;
    if (!size1.IsFullySpecified())
        size1 = {frame_->width, frame_->height };
    auto size = size1;
    if (size.GetWidth() & 0x0f) {
        size.SetWidth((size.GetWidth() & ~0x0f) + 0x10);
        if (size.GetWidth() != width_) {
            std::fill(bits_.begin(), bits_.end(), 0);
            width_ = size.GetWidth();
        }
    }
    AVPixelFormat wxFmt = AV_PIX_FMT_RGB24;
    sws_ctx_   = sws_getCachedContext(sws_ctx_,
                                    frame_->width, frame_->height, AVPixelFormat(frame_->format),
                                    size1.GetWidth(), size1.GetHeight(), wxFmt,
                                    SWS_GAUSS,
                                    nullptr, nullptr, nullptr);
    if (sws_ctx_ == nullptr)
        return false;
    int length = size.GetWidth() * size.GetHeight() * 3;
    if (bits_.size() < length)
        bits_.resize(length);
    uint8_t * datas[]   = { bits_.data() };
    int      strides[] = { size.GetWidth() * 3 };
    int      result_h = sws_scale(sws_ctx_, frame_->data, frame_->linesize, 0, frame_->height, datas, strides);
    if (result_h != size.GetHeight()) {
        return false;
    }
    // Copy: the frame outlives this decoder and is painted by the GUI thread while the
    // next sws_scale is already overwriting bits_, so it must own its pixels. The Windows
    // path below needs no equivalent, wxBitmap copies the bits into GDI.
    image = wxImage(size.GetWidth(), size.GetHeight(), bits_.data(), true).Copy();
    if (!image.IsOk()) {
        fprintf(stderr, "AVVideoDecoder: image not ok %dx%d\n", size.GetWidth(), size.GetHeight());
        return false;
    }
    return true;
}

bool AVVideoDecoder::toWxBitmap(wxBitmap &bitmap, wxSize const &size2)
{
    if (!got_frame_)
        return false;

    auto size1 = size2;
    if (!size1.IsFullySpecified())
        size1 = {frame_->width, frame_->height };
    auto size = size1;
    if (size.GetWidth() & 0x0f) {
        size.SetWidth((size.GetWidth() & ~0x0f) + 0x10);
        if (size.GetWidth() != width_) {
            std::fill(bits_.begin(), bits_.end(), 0);
            width_ = size.GetWidth();
        }
    }
    AVPixelFormat wxFmt = AV_PIX_FMT_RGB32;
    sws_ctx_ = sws_getCachedContext(sws_ctx_,
                                    frame_->width, frame_->height, AVPixelFormat(frame_->format),
                                    size1.GetWidth(), size1.GetHeight(), wxFmt,
                                    SWS_GAUSS,
                                    nullptr, nullptr, nullptr);
    if (sws_ctx_ == nullptr)
        return false;
    int length = size.GetWidth() * size.GetHeight() * 4;
    if (bits_.size() < length)
        bits_.resize(length);
    uint8_t *datas[]   = { bits_.data() };
    int      strides[] = { size.GetWidth() * 4 };
    int      result_h  = sws_scale(sws_ctx_, frame_->data, frame_->linesize, 0, frame_->height, datas, strides);
    if (result_h != size.GetHeight()) {
        fprintf(stderr, "AVVideoDecoder: result_h %d %d\n", result_h, size.GetHeight());
        return false;
    }
    bitmap = wxBitmap((char const *) bits_.data(), size.GetWidth(), size.GetHeight(), 32);
    assert(bitmap.IsOk());
    if (!bitmap.IsOk()) {
        fprintf(stderr, "AVVideoDecoder: bitmap not ok %dx%d\n", size.GetWidth(), size.GetHeight());
        return false;
    }
    return true;
}
