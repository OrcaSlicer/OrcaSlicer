#include "Flashforge.hpp"
#include <algorithm>
#include <ctime>
#include <chrono>
#include <thread>
#include <boost/filesystem/path.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/asio.hpp>
#include <boost/algorithm/string.hpp>

#include <wx/frame.h>
#include <wx/event.h>
#include <wx/progdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/checkbox.h>

#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "Http.hpp"
#include "TCPConsole.hpp"
#include "SerialMessage.hpp"
#include "SerialMessageType.hpp"

namespace fs = boost::filesystem;
namespace pt = boost::property_tree;

namespace Slic3r {

Flashforge::Flashforge(DynamicPrintConfig* config)
    : m_host(config->opt_string("print_host"))
    , m_console_port("8899")
    , m_gcFlavor(config->option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor")->value)
    , m_bufferSize(4096) // 4K buffer size
{}

const char* Flashforge::get_name() const { return "Flashforge"; }

bool Flashforge::test(wxString& msg) const
{
    BOOST_LOG_TRIVIAL(debug) << boost::format("[Flashforge Serial] testing connection");
    // Utils::TCPConsole console(m_host, m_console_port);
    Utils::TCPConsole client(m_host, m_console_port);
    client.enqueue_cmd(controlCommand);
    bool res = client.run_queue();
    if (!res) {
        msg = wxString::FromUTF8(client.error_message().c_str());
        BOOST_LOG_TRIVIAL(info) << boost::format("[Flashforge Serial] testing connection failed");
    } else {
        BOOST_LOG_TRIVIAL(info) << boost::format("[Flashforge Serial] testing connection success");
    }
    return res;
}

wxString Flashforge::get_test_ok_msg() const { return _(L("Serial connection to Flashforge is working correctly.")); }

wxString Flashforge::get_test_failed_msg(wxString& msg) const
{
    return GUI::from_u8((boost::format("%s: %s") % _utf8(L("Could not connect to Flashforge via serial")) % std::string(msg.ToUTF8())).str());
}


bool Flashforge::connect(wxString& msg) const
{
    
    Utils::TCPConsole client(m_host, m_console_port);

    client.enqueue_cmd(controlCommand);
    client.enqueue_cmd(deviceInfoCommand);

    if (m_gcFlavor == gcfKlipper)
        client.enqueue_cmd(connectKlipperCommand);
    else {
        client.enqueue_cmd(connectLegacyCommand);

    }

    client.enqueue_cmd(statusCommand);
    

    bool res = client.run_queue();

    if (!res) {
        msg = wxString::FromUTF8(client.error_message().c_str());
        BOOST_LOG_TRIVIAL(info) << boost::format("[Flashforge Serial] Failed to initiate connection");
    } else
        BOOST_LOG_TRIVIAL(info) << boost::format("[Flashforge Serial] Successfully initiated Connection");

    return res;
}

bool Flashforge::start_print(wxString& msg, const std::string& filename) const
{
    Utils::TCPConsole            client(m_host, m_console_port);
    Slic3r::Utils::SerialMessage startPrintCommand = {(boost::format("~M23 0:/user/%1%") % filename).str(), Slic3r::Utils::Command};
    client.enqueue_cmd(startPrintCommand);
    bool res = client.run_queue();

    if (!res) {
        msg = wxString::FromUTF8(client.error_message().c_str());
        BOOST_LOG_TRIVIAL(info) << boost::format("[Flashforge Serial] Failed to start print %1%") % filename;
    } else
        BOOST_LOG_TRIVIAL(info) << boost::format("[Flashforge Serial] Started print %1%") % filename;

    return res;
}

bool Flashforge::upload(PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn, InfoFn info_fn) const
{
    bool res = true;
    wxString errormsg;

    Utils::TCPConsole client(m_host, m_console_port);

    try {

        res = connect(errormsg);

        std::ifstream newfile;
        newfile.open(upload_data.source_path.c_str(), std::ios::binary); // open a file to perform read operation using file object
        std::string gcodeFile;
        if (newfile.is_open()) {                                         // checking whether the file is open
            BOOST_LOG_TRIVIAL(info) << boost::format("[Flashforge Serial] Reading file...");
            newfile.seekg(0, std::ios::end);
            std::ifstream::pos_type pos = newfile.tellg();

            std::vector<char> result(pos);

            newfile.seekg(0, std::ios::beg);
            newfile.read(&result[0], pos);

            gcodeFile = std::string(result.begin(), result.end()); // TODO: Find more efficient way of breaking ifstream

            BOOST_LOG_TRIVIAL(info) << boost::format("[Flashforge Serial] Reading file...done size is %1%") % gcodeFile.size();

            newfile.close(); // close the file object.
        }
        Slic3r::Utils::SerialMessage fileuploadCommand =
            {(boost::format("~M28 %1% 0:/user/%2%") % gcodeFile.size() % upload_data.upload_path.generic_string()).str(),
             Slic3r::Utils::Command};
        client.enqueue_cmd(fileuploadCommand);

        //client.set_tcp_queue_delay(std::chrono::nanoseconds(10000));

        for (int bytePos = 0; bytePos < gcodeFile.size(); bytePos += m_bufferSize) { // TODO: Find more efficient way of breaking ifstream

            int bytePosEnd  = (gcodeFile.size() - bytePos > m_bufferSize - 1) ? m_bufferSize : gcodeFile.size();
            Slic3r::Utils::SerialMessage dataCommand = {gcodeFile.substr(bytePos, bytePosEnd), Slic3r::Utils::Data}; // Break into smaller byte chunks

            client.enqueue_cmd(dataCommand);

        }

        res = client.run_queue();

        if (res)
            BOOST_LOG_TRIVIAL(info) << boost::format("[Flashforge Serial] Sent %1% ") % gcodeFile.size();


        if (!res) {
            BOOST_LOG_TRIVIAL(info) << boost::format("[Flashforge Serial] error %1%") % client.error_message().c_str();
            errormsg = wxString::FromUTF8(client.error_message().c_str());
            error_fn(std::move(errormsg));
        } else {

            client.set_tcp_queue_delay(std::chrono::milliseconds(3000));

            BOOST_LOG_TRIVIAL(info) << boost::format("[Flashforge Serial] Sending file save command ");
            
            client.enqueue_cmd(saveFileCommand);

            res = client.run_queue();

            if (upload_data.post_action == PrintHostPostUploadAction::StartPrint)
                res = start_print(errormsg, upload_data.upload_path.string());
        }

    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(info) << boost::format("[Flashforge Serial] error %1%") % e.what();
        errormsg = wxString::FromUTF8(e.what());
        error_fn(std::move(errormsg));
    }

    return res;
}

int Flashforge::get_err_code_from_body(const std::string& body) const
{
    pt::ptree          root;
    std::istringstream iss(body); // wrap returned json to istringstream
    pt::read_json(iss, root);

    return root.get<int>("err", 0);
}

} // namespace Slic3r
