#include "gateway/business.hpp"

namespace gateway {

realtime::Result validate_request(const realtime::Request& request) {
    if (request.request_id() == 0 || request.device_id().empty() ||
        request.device_id().size() > 128) {
        return realtime::INVALID_REQUEST;
    }
    if (request.message_type() == realtime::HEARTBEAT) {
        if (!request.payload().empty()) {
            return realtime::INVALID_REQUEST;
        }
        return realtime::OK;
    }
    // 保留旧错误优先级：非心跳空 payload 先报 INVALID_REQUEST。
    if (request.payload().empty()) {
        return realtime::INVALID_REQUEST;
    }
    if (request.message_type() != realtime::TELEMETRY) {
        return realtime::UNKNOWN_MESSAGE_TYPE;
    }
    return realtime::OK;
}

realtime::Ack Router::route(const realtime::Request& request) {
    realtime::Ack ack;
    ack.set_request_id(request.request_id());
    ack.set_result(validate_request(request));
    if (ack.result() == realtime::OK) {
        if (request.message_type() == realtime::HEARTBEAT) {
            stats_.heartbeats.fetch_add(1, std::memory_order_relaxed);
        } else {
            stats_.payload_bytes.fetch_add(request.payload().size(), std::memory_order_relaxed);
            stats_.processed.fetch_add(1, std::memory_order_relaxed);
        }
        return ack;
    }
    if (ack.result() == realtime::UNKNOWN_MESSAGE_TYPE) {
        ack.set_error("unsupported message_type");
    } else {
        ack.set_error("invalid request_id, device_id or payload for message_type");
    }
    stats_.rejected.fetch_add(1, std::memory_order_relaxed);
    return ack;
}

} // namespace gateway
