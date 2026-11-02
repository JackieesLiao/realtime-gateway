#pragma once

#include "gateway/bounded_task_queue.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace gateway {

struct Config {
    std::string listen_address = "127.0.0.1";
    std::uint16_t port = 9000;
    std::size_t io_threads = 2;
    std::size_t max_connections = 1024;
    std::size_t business_threads = 2;
    TaskQueueLimits business_queue;
    std::size_t max_body_bytes = 1048576;
    std::size_t max_receive_body_bytes = 1048576;
    std::size_t max_send_messages = 64;
    std::size_t max_send_bytes = 262144;

    static Config load(const std::string& path);
};

} // namespace gateway
