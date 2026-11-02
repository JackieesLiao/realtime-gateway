#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>

namespace gateway {
std::string encode_frame(const std::string& body);
// One incomplete frame only; call the consumer immediately for each complete body.
class FrameDecoder {
public:
    using Consumer = std::function<bool(const std::string&)>;
    FrameDecoder(std::size_t max_body, std::size_t receive_capacity);
    bool feed(std::span<const char> bytes, const Consumer& consume);
    const std::string& error() const { return error_; }
    std::size_t buffered_body_bytes() const { return body_.size(); }
    std::size_t allocated_body_capacity() const { return body_.capacity(); }
private:
    std::size_t max_body_, receive_capacity_;
    std::array<unsigned char, 4> header_{};
    std::size_t header_used_ = 0, expected_ = 0;
    std::string body_, error_;
};
}
