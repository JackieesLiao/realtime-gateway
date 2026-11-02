#include "gateway/config.hpp"

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

    return config;
}

} // namespace gateway
