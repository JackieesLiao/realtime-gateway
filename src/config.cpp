#include "gateway/config.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace gateway {

Config Config::load(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("cannot open config: " + path);
    }

    nlohmann::json document;
    file >> document;
    if (!document.is_object()) {
        throw std::runtime_error("config must be object");
    }

    Config config;
    for (const auto& [key, value] : document.items()) {
        if (key == "log_level") {
            config.log_level = value.get<std::string>();
            if (config.log_level != "info" && config.log_level != "debug") {
                throw std::runtime_error("log_level must be info or debug");
            }
            continue;
        }

        if (key == "listen_address") {
            config.listen_address = value.get<std::string>();
            continue;
        }

        if (!value.is_number_integer() || value.get<std::int64_t>() < 0) {
            throw std::runtime_error("invalid config field: " + key);
        }

        const auto numeric_value = value.get<std::uint64_t>();
        const auto require_range = [&](std::uint64_t minimum, std::uint64_t maximum) {
            if (numeric_value < minimum || numeric_value > maximum) {
                throw std::runtime_error("out of range: " + key);
            }
        };

        if (key == "port") {
            require_range(0, 65535);
            config.port = static_cast<std::uint16_t>(numeric_value);
        } else if (key == "io_threads") {
            require_range(1, 64);
            config.io_threads = numeric_value;
        } else if (key == "max_connections") {
            require_range(1, 100000);
            config.max_connections = numeric_value;
        } else if (key == "business_threads") {
            require_range(1, 64);
            config.business_threads = numeric_value;
        } else if (key == "max_business_queue_messages") {
            require_range(1, 65536);
            config.business_queue.queued_messages = numeric_value;
        } else if (key == "max_business_queue_bytes") {
            require_range(1, 256 * 1024 * 1024);
            config.business_queue.queued_bytes = numeric_value;
        } else if (key == "max_business_outstanding_messages") {
            require_range(1, 65536);
            config.business_queue.outstanding_messages = numeric_value;
        } else if (key == "max_business_outstanding_bytes") {
            require_range(1, 256 * 1024 * 1024);
            config.business_queue.outstanding_bytes = numeric_value;
        } else if (key == "rate_messages_per_second") {
            require_range(0, 1000000000);
            config.connection.rate.messages_per_second = numeric_value;
        } else if (key == "rate_message_burst") {
            require_range(1, 1024 * 1024 * 1024);
            config.connection.rate.message_burst = numeric_value;
        } else if (key == "rate_bytes_per_second") {
            require_range(0, 1000000000);
            config.connection.rate.bytes_per_second = numeric_value;
        } else if (key == "rate_byte_burst") {
            require_range(1, 1024 * 1024 * 1024);
            config.connection.rate.byte_burst = numeric_value;
        } else if (key == "idle_timeout_ms" || key == "frame_timeout_ms" ||
                   key == "wait_timeout_ms" || key == "write_timeout_ms") {
            require_range(0, 3600000);
            const std::chrono::milliseconds duration(static_cast<std::int64_t>(numeric_value));
            if (key == "idle_timeout_ms") {
                config.connection.timeout.idle = duration;
            } else if (key == "frame_timeout_ms") {
                config.connection.timeout.frame = duration;
            } else if (key == "wait_timeout_ms") {
                config.connection.timeout.wait = duration;
            } else {
                config.connection.timeout.write = duration;
            }
        } else if (key == "global_receive_messages") {
            require_range(1, 1000000);
            config.global_receive.messages = numeric_value;
        } else if (key == "global_receive_bytes") {
            require_range(1, 16ULL * 1024 * 1024 * 1024);
            config.global_receive.bytes = numeric_value;
        } else if (key == "global_send_messages") {
            require_range(1, 1000000);
            config.global_send.messages = numeric_value;
        } else if (key == "global_send_bytes") {
            require_range(1, 16ULL * 1024 * 1024 * 1024);
            config.global_send.bytes = numeric_value;
        } else if (key == "global_budget_messages") {
            require_range(3, 1000000);
            config.global_budget.messages = numeric_value;
        } else if (key == "global_budget_bytes") {
            require_range(1, 16ULL * 1024 * 1024 * 1024);
            config.global_budget.bytes = numeric_value;
        } else if (key == "backpressure_high_percent") {
            require_range(1, 99);
            config.backpressure_high_percent = static_cast<unsigned>(numeric_value);
        } else if (key == "backpressure_low_percent") {
            require_range(0, 98);
            config.backpressure_low_percent = static_cast<unsigned>(numeric_value);
        } else if (key == "max_body_bytes") {
            require_range(1, 16 * 1024 * 1024);
            config.max_body_bytes = numeric_value;
        } else if (key == "max_receive_body_bytes") {
            require_range(1, 16 * 1024 * 1024);
            config.max_receive_body_bytes = numeric_value;
        } else if (key == "max_send_messages") {
            require_range(1, 65536);
            config.max_send_messages = numeric_value;
        } else if (key == "max_send_bytes") {
            require_range(1, 64 * 1024 * 1024);
            config.max_send_bytes = numeric_value;
        } else {
            throw std::runtime_error("unknown config field: " + key);
        }
    }

    // 统一校验水位关系和固定读块的全局保留空间，不分配网络缓冲。
    ResourceBudget validated(config.budget_limits());
    ConnectionPolicy policy(config.connection, PolicyClock::now());
    return config;
}

ResourceBudgetLimits Config::budget_limits() const {
    ResourceBudgetLimits limits;
    limits.connections = max_connections;
    limits.receive_body_bytes = std::min(max_body_bytes, max_receive_body_bytes);
    limits.receive = global_receive;
    // 队列仍按请求字节计费；统一预算额外预留接收规范化余量和并存的 ACK 对象。
    const auto completion_headroom =
        business_queue.outstanding_messages * (request_reserve_padding + max_ack_frame_bytes);
    limits.business = {business_queue.outstanding_messages,
        business_queue.outstanding_bytes + completion_headroom};
    limits.local_send = {max_send_messages, max_send_bytes};
    limits.send = global_send;
    limits.total = global_budget;
    limits.high_percent = backpressure_high_percent;
    limits.low_percent = backpressure_low_percent;
    return limits;
}

TaskQueueLimits Config::task_queue_limits() const {
    auto limits = business_queue;
    limits.high_percent = backpressure_high_percent;
    limits.low_percent = backpressure_low_percent;
    return limits;
}

} // namespace gateway
