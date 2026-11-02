#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
namespace gateway {
struct Config {
    std::string listen_address = "127.0.0.1";
    std::uint16_t port = 9000;
    std::size_t io_threads = 2, max_connections = 1024;
    std::size_t max_body_bytes = 1048576, max_receive_body_bytes = 1048576;
    std::size_t max_send_messages = 64, max_send_bytes = 262144;
    static Config load(const std::string& path);
};
}
