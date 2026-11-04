#pragma once

#include "gateway.pb.h"

#include <atomic>
#include <cstdint>

namespace gateway {

// Shared by connections on different strands; no counter publishes other shared state.
struct Statistics {
    std::atomic<std::uint64_t> processed{0};
    std::atomic<std::uint64_t> rejected{0};
    std::atomic<std::uint64_t> heartbeats{0};
    std::atomic<std::uint64_t> payload_bytes{0};
};

// Transport 用相同的轻量校验判断有效活动；不复制业务字段约束。
realtime::Result validate_request(const realtime::Request& request);

class Router {
public:
    explicit Router(Statistics& stats) : stats_(stats) {
    }

    realtime::Ack route(const realtime::Request& request);

private:
    Statistics& stats_;
};

} // namespace gateway
