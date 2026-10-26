#pragma once

#include <functional>
#include <string>
#include <vector>

namespace Slic3r {

struct CameraIceServer {
    std::string urls;
    std::string username;
    std::string credential;
};

enum class CameraUnavailableReason {
    Busy,
    Error,
    Disabled,
    Closed,
};

class ICameraSignalingChannel {
public:
    virtual ~ICameraSignalingChannel() = default;

    virtual void open() = 0;
    virtual void close() = 0;
    virtual void send_offer(std::string sdp) = 0;
    virtual void send_ice(std::string candidate, std::string mid) = 0;

    // These callbacks are invoked by the channel's worker thread. Consumers
    // must marshal UI work to the GUI thread themselves.
    std::function<void(std::vector<CameraIceServer>)> on_ready;
    std::function<void(std::string)> on_answer;
    std::function<void(std::string, std::string)> on_ice;
    std::function<void(CameraUnavailableReason, std::string)> on_unavailable;
};

} // namespace Slic3r
