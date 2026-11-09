#pragma once

#include "PrinterNetworkTypes.hpp"
#include <string>

namespace Slic3r {

// A camera source is explicit: constructing one never changes the selected Device printer.
struct BambuCameraSource
{
    LiveviewLocal protocol = LVL_Local;
    std::string host;
    std::string user = "bblp";
    std::string password;
    std::string device;
    std::string network_version;
    std::string device_version;
    std::string client_id;
    std::string client_version;

    bool same_connection(const BambuCameraSource& other) const
    {
        // Version and client metadata can arrive after playback starts. They describe a
        // connection, but must not tear down a working feed when status initialization finishes.
        return protocol == other.protocol && host == other.host && user == other.user &&
               password == other.password && device == other.device;
    }

    std::string url() const
    {
        std::string result;
        if (protocol == LVL_Local)
            result = "bambu:///local/" + host + ".?port=6000&user=" + user + "&passwd=" + password;
        else if (protocol == LVL_Rtsps)
            result = "bambu:///rtsps___" + user + ":" + password + "@" + host + "/streaming/live/1?proto=rtsps";
        else if (protocol == LVL_Rtsp)
            result = "bambu:///rtsp___" + user + ":" + password + "@" + host + "/streaming/live/1?proto=rtsp";
        else
            return {};
        result += "&device=" + device;
        result += "&dev_ver=" + device_version;
        if (!network_version.empty()) result += "&net_ver=" + network_version;
        if (!client_id.empty()) result += "&cli_id=" + client_id;
        if (!client_version.empty()) result += "&cli_ver=" + client_version;
        return result;
    }
};

} // namespace Slic3r
