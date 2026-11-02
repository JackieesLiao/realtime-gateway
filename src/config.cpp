#include "gateway/config.hpp"
#include <fstream>
#include <stdexcept>
#include <nlohmann/json.hpp>
namespace gateway {
Config Config::load(const std::string& path) {
    std::ifstream file(path);
    if (!file) throw std::runtime_error("cannot open config: " + path);
    nlohmann::json j; file >> j;
    if (!j.is_object()) throw std::runtime_error("config must be object");
    Config c;
    for (const auto& [key, value] : j.items()) {
        if (key == "listen_address") { c.listen_address = value.get<std::string>(); continue; }
        if (!value.is_number_integer() || value.get<std::int64_t>() < 0)
            throw std::runtime_error("invalid config field: " + key);
        const auto n = value.get<std::uint64_t>();
        const auto check = [&](std::uint64_t lo, std::uint64_t hi) {
            if (n < lo || n > hi) throw std::runtime_error("out of range: " + key);
        };
        if (key == "port") { check(0, 65535); c.port = static_cast<std::uint16_t>(n); }
        else if (key == "io_threads") { check(1, 64); c.io_threads = n; }
        else if (key == "max_connections") { check(1, 100000); c.max_connections = n; }
        else if (key == "max_body_bytes") { check(1, 16 * 1024 * 1024); c.max_body_bytes = n; }
        else if (key == "max_receive_body_bytes") { check(1, 16 * 1024 * 1024); c.max_receive_body_bytes = n; }
        else if (key == "max_send_messages") { check(1, 65536); c.max_send_messages = n; }
        else if (key == "max_send_bytes") { check(1, 64 * 1024 * 1024); c.max_send_bytes = n; }
        else throw std::runtime_error("unknown config field: " + key);
    }
    return c;
}
}
