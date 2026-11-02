#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>

namespace gateway {

std::string encode_frame(const std::string& body);

// Accumulate only one incomplete frame; consume complete frames immediately.
class FrameDecoder {
public:
    using Consumer = std::function<bool(const std::string&)>;

    FrameDecoder(std::size_t max_body_bytes, std::size_t receive_body_capacity);

    bool feed(std::span<const char> remaining_bytes, const Consumer& consume_body);

    const std::string& error() const {
        return error_;
    }

    std::size_t buffered_body_bytes() const {
        return body_.size();
    }

    std::size_t allocated_body_capacity() const {
        return body_.capacity();
    }

private:
    std::size_t max_body_bytes_;
    std::size_t receive_body_capacity_;
    std::array<unsigned char, 4> header_{};
    std::size_t header_bytes_received_ = 0;
    std::size_t expected_body_bytes_ = 0;
    std::string body_;
    std::string error_;
};

} // namespace gateway
