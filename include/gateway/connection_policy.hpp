#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace gateway {

using PolicyClock = std::chrono::steady_clock;
using PolicyTime = PolicyClock::time_point;
using PeerTime = PolicyClock::duration;

struct RateLimits {
    std::uint64_t messages_per_second = 10000;
    std::uint64_t message_burst = 1000;
    std::uint64_t bytes_per_second = 16 * 1024 * 1024;
    std::uint64_t byte_burst = 16 * 1024 * 1024;
};

struct TimeoutLimits {
    // 0 表示禁用该项期限；生产默认值是示例策略，不代表性能目标。
    std::chrono::milliseconds idle{60000};
    std::chrono::milliseconds frame{10000};
    std::chrono::milliseconds wait{5000};
    std::chrono::milliseconds write{5000};
};

struct ConnectionLimits {
    RateLimits rate;
    TimeoutLimits timeout;
};

// 只在连接 strand 使用；整数定点令牌保留不足一个令牌的补充量。
class TokenBucket {
public:
    TokenBucket(std::uint64_t rate, std::uint64_t burst, PolicyTime now);
    bool can_ever_fit(std::size_t cost) const;
    PolicyClock::duration delay(std::size_t cost, PolicyTime now);
    void consume(std::size_t cost, PolicyTime now);

private:
    void refill(PolicyTime now);

    std::uint64_t rate_;
    std::uint64_t capacity_;
    std::uint64_t credit_;
    PolicyTime updated_;
};

struct RateDecision {
    bool possible;
    PolicyTime retry_at;
};

enum class WaitReason { None, Rate, Resource };
enum class TimeoutReason { None, Idle, Frame, RateWait, ResourceWait, Write };
const char* timeout_name(TimeoutReason reason);

// M2.4：纯时间/限流状态，不持 socket 或线程，也不读取系统时钟。
// 调用方显式提供 now，单测因此能精确覆盖阈值和长时间暂停。
class ConnectionPolicy {
public:
    ConnectionPolicy(ConnectionLimits limits, PolicyTime now);

    RateDecision check_rate(std::size_t body_bytes, PolicyTime now);
    void consume_rate(std::size_t body_bytes, PolicyTime now);
    void wait_for(WaitReason reason, PolicyTime now);
    void wait_for_rate(PolicyTime retry_at, PolicyTime now);
    void finish_wait();
    bool take_rate_wakeup(PolicyTime now);

    // peer_time 是扣除服务端暂停后的时间轴；读块记录这个值供尾部帧复用。
    PeerTime peer_time(PolicyTime now) const;
    void pause_peer(bool paused, PolicyTime now);
    void frame_started(PeerTime first_byte);
    void frame_completed();
    // 使用完整帧最后一个读块的到达时间，排队后再解析不能伪造新的活动。
    void valid_request(PolicyTime received_at);
    void write_started(PolicyTime now);
    void write_finished();

    TimeoutReason expired(PolicyTime now) const;
    std::optional<PolicyTime> next_deadline(PolicyTime now) const;

private:
    ConnectionLimits limits_;
    TokenBucket messages_;
    TokenBucket bytes_;
    PolicyTime created_;
    PeerTime paused_total_{};
    std::optional<PolicyTime> paused_at_;
    PolicyTime last_valid_;
    std::optional<PeerTime> frame_at_;
    std::optional<PolicyTime> wait_at_;
    std::optional<PolicyTime> write_at_;
    std::optional<PolicyTime> rate_retry_;
    WaitReason waiting_ = WaitReason::None;
};

} // namespace gateway
