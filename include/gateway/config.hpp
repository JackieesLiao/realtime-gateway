#pragma once

#include "gateway/bounded_task_queue.hpp"
#include "gateway/connection_policy.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace gateway {

struct Config {
    std::string log_level = "info";
    std::string listen_address = "127.0.0.1";
    std::uint16_t port = 9000;
    std::size_t io_threads = 2;
    std::size_t max_connections = 1024;
    std::size_t business_threads = 2;
    TaskQueueLimits business_queue;
    ConnectionLimits connection;
    ResourceUsage global_receive{256, 64 * 1024 * 1024};
    ResourceUsage global_send{4096, 16 * 1024 * 1024};
    ResourceUsage global_budget{8192, 128 * 1024 * 1024};
    unsigned backpressure_high_percent = 80;
    unsigned backpressure_low_percent = 50;
    std::size_t max_body_bytes = 1048576;
    std::size_t max_receive_body_bytes = 1048576;
    std::size_t max_send_messages = 64;
    std::size_t max_send_bytes = 262144;

    static Config load(const std::string& path);
    ResourceBudgetLimits budget_limits() const;
    TaskQueueLimits task_queue_limits() const;
};

} // namespace gateway
