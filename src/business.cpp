#include "gateway/business.hpp"

namespace gateway {

realtime::Ack Router::route(const realtime::Request& request) {
    realtime::Ack ack;
    ack.set_request_id(request.request_id());

    const bool invalid_fields = request.request_id() == 0 || request.device_id().empty() ||
                                request.device_id().size() > 128 || request.payload().empty();

    if (invalid_fields) {
        ack.set_result(realtime::INVALID_REQUEST);
        ack.set_error("request_id, device_id(1..128 bytes), payload required");
    } else {
        switch (request.message_type()) {
        case realtime::TELEMETRY:
            stats_.payload_bytes.fetch_add(request.payload().size(), std::memory_order_relaxed);
            stats_.processed.fetch_add(1, std::memory_order_relaxed);
            ack.set_result(realtime::OK);
            return ack;

        default:
            ack.set_result(realtime::UNKNOWN_MESSAGE_TYPE);
            ack.set_error("unsupported message_type");
        }
    }

    stats_.rejected.fetch_add(1, std::memory_order_relaxed);
    return ack;
}

} // namespace gateway
