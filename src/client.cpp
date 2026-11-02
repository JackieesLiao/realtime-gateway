#include <utility>
#include <boost/asio.hpp>
#include "gateway/frame.hpp"
#include "gateway.pb.h"
#include <array>
#include <iostream>
int main(int argc, char** argv) {
    if (argc < 3 || argc > 4) {
        std::cerr << "usage: device_client HOST PORT [REQUEST_ID]\n"; return 2;
    }
    try {
        namespace asio = boost::asio;
        using tcp = asio::ip::tcp;
        const auto id = argc == 4 ? std::stoull(argv[3]) : 1ULL;
        if (id == 0) throw std::runtime_error("request_id must be nonzero");
        asio::io_context io;
        tcp::resolver resolver(io);
        tcp::socket socket(io);
        asio::connect(socket, resolver.resolve(argv[1], argv[2]));
        realtime::Request request;
        request.set_request_id(id);
        request.set_device_id("simulator-1");
        request.set_message_type(realtime::TELEMETRY);
        request.set_payload("position=1,2,3");
        const auto wire = gateway::encode_frame(request.SerializeAsString());
        asio::write(socket, asio::buffer(wire));
        std::array<unsigned char, 4> header{};
        asio::read(socket, asio::buffer(header));
        std::uint32_t size = 0;
        for (const auto b : header) size = (size << 8) | b;
        if (size == 0 || size > 1048576) throw std::runtime_error("invalid ACK frame length");
        std::string body(size, '\0');
        asio::read(socket, asio::buffer(body));
        realtime::Ack ack;
        if (!ack.ParseFromString(body) || ack.request_id() != id) throw std::runtime_error("invalid ACK");
        std::cout << "request_id=" << ack.request_id() << " result=" << realtime::Result_Name(ack.result())
                  << " error=" << ack.error() << '\n';
        return ack.result() == realtime::OK ? 0 : 1;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
