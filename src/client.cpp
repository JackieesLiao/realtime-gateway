#include "gateway/frame.hpp"
#include "gateway.pb.h"

#include <array>
#include <iostream>
#include <stdexcept>
#include <utility>

#include <boost/asio.hpp>

int main(int argc, char** argv) {
    if (argc < 3 || argc > 5) {
        std::cerr << "usage: device_client HOST PORT [REQUEST_ID] [telemetry|heartbeat]\n";
        return 2;
    }

    try {
        namespace asio = boost::asio;
        using tcp = asio::ip::tcp;

        std::uint64_t request_id = 1;
        if (argc >= 4) {
            request_id = std::stoull(argv[3]);
        }
        if (request_id == 0) {
            throw std::runtime_error("request_id must be nonzero");
        }

        asio::io_context io;
        tcp::resolver resolver(io);
        tcp::socket socket(io);
        asio::connect(socket, resolver.resolve(argv[1], argv[2]));

        realtime::Request request;
        request.set_request_id(request_id);
        request.set_device_id("simulator-1");
        request.set_message_type(realtime::TELEMETRY);
        request.set_payload("position=1,2,3");
        if (argc == 5) {
            const std::string mode(argv[4]);
            if (mode == "heartbeat") {
                request.set_message_type(realtime::HEARTBEAT);
                request.clear_payload();
            } else if (mode != "telemetry") {
                throw std::runtime_error("mode must be telemetry or heartbeat");
            }
        }

        const auto request_frame = gateway::encode_frame(request.SerializeAsString());
        asio::write(socket, asio::buffer(request_frame));

        std::array<unsigned char, 4> header{};
        asio::read(socket, asio::buffer(header));

        std::uint32_t ack_body_length = 0;
        for (const auto header_byte : header) {
            ack_body_length = (ack_body_length << 8) | header_byte;
        }
        if (ack_body_length == 0 || ack_body_length > 1048576) {
            throw std::runtime_error("invalid ACK frame length");
        }

        std::string ack_body(ack_body_length, '\0');
        asio::read(socket, asio::buffer(ack_body));

        realtime::Ack ack;
        if (!ack.ParseFromString(ack_body) || ack.request_id() != request_id) {
            throw std::runtime_error("invalid ACK");
        }

        std::cout << "request_id=" << ack.request_id()
                  << " result=" << realtime::Result_Name(ack.result()) << " error=" << ack.error()
                  << '\n';

        if (ack.result() != realtime::OK) {
            return 1;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
