#pragma once
#include "gateway.pb.h"
#include <atomic>
namespace gateway {
struct Statistics {
    std::atomic<std::uint64_t> processed{0}, rejected{0}, payload_bytes{0};
};
class Router {
public:
    explicit Router(Statistics& stats) : stats_(stats) {}
    realtime::Ack route(const realtime::Request& request);
private:
    Statistics& stats_;
};
// M1 executes only constant/linear bounded validation and counters inline.
// No waiting business tasks. Worker submission and completion executor belong to M2.
class Dispatcher {
public:
    explicit Dispatcher(Statistics& stats) : router_(stats) {}
    realtime::Ack dispatch(const realtime::Request& request) { return router_.route(request); }
private:
    Router router_;
};
}
