#pragma once

#include "IMediaController.hpp"
#include <slic3r/Utils/IPrinterAgent.hpp>

#include <wx/image.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace rtc {
class DataChannel;
class PeerConnection;
}

namespace Slic3r { namespace GUI {

class WebRtcMediaController : public IMediaController {
public:
    struct Status {
        enum Kind { Connecting, Playing, Stopped, Failed } kind = Stopped;
        enum Code {
            ICE_FAILED,
            SIGNALING_CLOSED,
            UNAVAILABLE_BUSY,
            UNAVAILABLE_ERROR,
            UNAVAILABLE_DISABLED,
            DECODE_ERROR,
            TIMEOUT,
        } code = ICE_FAILED;
        // Identifies the Play attempt this status belongs to, so the
        // consumer can drop CallAfter-queued events from a superseded attempt.
        std::uint64_t epoch = 0;
    };

    WebRtcMediaController(std::function<void(const wxImage&, wxSize)> frame_sink,
                          std::function<void(Status)> on_status);
    ~WebRtcMediaController() override;

    void set_signalling_channel(std::unique_ptr<ICameraSignalingChannel> channel);
    std::uint64_t epoch() const { return m_epoch.load(); }
    bool is_active() const { return m_alive.load(); }

    void Load(wxURI) override {}
    void Play() override;
    void Stop() override;
    wxMediaState GetState() override;
    wxSize GetVideoSize() const override;

private:
    void teardown(bool notify);
    void report(Status status);
    void bind_data_channel(const std::shared_ptr<rtc::DataChannel>& dc);
    void on_ready(std::vector<CameraIceServer> servers);
    void on_answer(std::string sdp);
    void on_ice(std::string candidate, std::string mid);
    void on_unavailable(CameraUnavailableReason reason, std::string detail);
    void decode_loop();
    void enqueue_jpeg(std::vector<std::byte> jpeg);
    void deliver_jpeg(std::vector<std::byte> jpeg);

    mutable std::mutex m_mutex;
    std::condition_variable m_cond;
    std::deque<std::vector<std::byte>> m_jpeg_queue;
    // Remote candidates can arrive before the answer; libdatachannel rejects
    // addRemoteCandidate until a remote description is set, so buffer them.
    std::vector<std::pair<std::string, std::string>> m_pending_candidates;
    bool m_remote_description_set = false;
    std::unique_ptr<ICameraSignalingChannel> m_pending_signaling;
    std::unique_ptr<ICameraSignalingChannel> m_signaling;
    std::shared_ptr<rtc::PeerConnection> m_peer_connection;
    std::shared_ptr<rtc::DataChannel> m_data_channel;
    std::thread m_decode_thread;
    std::atomic<bool> m_alive{false};
    std::atomic<std::uint64_t> m_epoch{0};
    wxMediaState m_state = static_cast<wxMediaState>(3);
    wxSize m_video_size = wxDefaultSize;
    std::function<void(const wxImage&, wxSize)> m_frame_sink;
    std::function<void(Status)> m_on_status;
    bool m_has_frame = false;
    std::chrono::steady_clock::time_point m_last_frame_time{};
};

}} // namespace Slic3r::GUI
