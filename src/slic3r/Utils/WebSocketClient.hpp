#ifndef _WEB_SOCKET_CLIENT_HPP_
#define _WEB_SOCKET_CLIENT_HPP_
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <iostream>
#include <string>
#include <chrono>
namespace beast = boost::beast;         // from <boost/beast.hpp>
namespace http = beast::http;           // from <boost/beast/http.hpp>
namespace websocket = beast::websocket; // from <boost/beast/websocket.hpp>
namespace net = boost::asio;            // from <boost/asio.hpp>
using tcp = net::ip::tcp;               // from <boost/asio/ip/tcp.hpp>

class WebSocketClient {
public:
//服务器是ws://echo.websocket.org:80/websocket
    WebSocketClient():
    resolver_(ioc_), ws_(ioc_),is_connect(false) {

    }

    ~WebSocketClient() {
        if(!is_connect){
            return;
        }
        try {
            // Close the WebSocket connection
            ws_.close(websocket::close_code::normal);
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << std::endl;
        }
    }
    void connect(const std::string& host, const std::string& port, const std::string& path="/"){
        if(is_connect){
           return;
        }
        // Look up the domain name
        auto const results = resolver_.resolve(host, port);

        // Make the connection on the IP address we get from a lookup
        auto ep = net::connect(ws_.next_layer(), results);
        std::string _host = host;
        //if _host last char is  '/', remove it
        if(_host.size()>0&&_host[host.size()-1] == '/'){
            _host[host.size()-1] = '\0';
        }

        // _host += ':' + std::to_string(ep.port());
        // Set a decorator to change the User-Agent of the handshake
        ws_.set_option(websocket::stream_base::decorator(
            [](websocket::request_type& req)
            {
                req.set(http::field::user_agent,"ElegooSlicer");
            }));
        // Perform the WebSocket handshake
        ws_.handshake(_host, path);
        is_connect = true;
    }

    void send(const std::string& message){
        // Send a message
        ws_.write(net::buffer(message));
    }

    std::string receive(int timeout = 0){
        // This buffer will hold the incoming message
        beast::flat_buffer buffer;

        // Read a message into our buffer
        ws_.read(buffer);

        // Return the message as a string
        return beast::buffers_to_string(buffer.data());
    }


private:
    net::io_context ioc_;
    tcp::resolver resolver_;
    websocket::stream<tcp::socket> ws_;
    bool is_connect;
};

// int main() {
//     try {
//         WebSocketClient client("echo.websocket.org", "80");

//         client.send("Hello, world!");
//         std::string response = client.receive();

//         std::cout << "Received: " << response << std::endl;
//     } catch (const std::exception& e) {
//         std::cerr << "Error: " << e.what() << std::endl;
//         return EXIT_FAILURE;
//     }

//     return EXIT_SUCCESS;
// }
#endif // _WEB_SOCKET_CLIENT_HPP