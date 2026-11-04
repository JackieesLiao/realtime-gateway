#include "gateway/connection_policy.hpp"

#include <algorithm>
#include <stdexcept>

namespace gateway {
namespace {

constexpr std::uint64_t units_per_token = 1000000000;
constexpr std::uint64_t maximum_rate = 1000000000;
constexpr std::uint64_t maximum_burst = 1024 * 1024 * 1024;

std::uint64_t divide_up(std::uint64_t numerator, std::uint64_t denominator) {
    return numerator / denominator + static_cast<std::uint64_t>(numerator % denominator != 0);
}

bool elapsed(PolicyTime start, std::chrono::milliseconds limit, PolicyTime now) {
    return limit.count() > 0 && now - start >= limit;
}

} // namespace

TokenBucket::TokenBucket(std::uint64_t rate, std::uint64_t burst, PolicyTime now)
    : rate_(rate), capacity_(0), credit_(0), updated_(now) {
    if (rate > maximum_rate || burst == 0 || burst > maximum_burst) {
        throw std::invalid_argument("invalid token bucket rate or burst");
    }
    capacity_ = burst * units_per_token;
    credit_ = capacity_;
}

bool TokenBucket::can_ever_fit(std::size_t cost) const {
    return rate_ == 0 || cost <= capacity_ / units_per_token;
}

void TokenBucket::refill(PolicyTime now) {
    if (now < updated_) {
        throw std::logic_error("token bucket time cannot move backwards");
    }
    const auto nanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - updated_).count();
    updated_ = now;
    if (rate_ == 0) {
        return;
    }
    const auto missing = capacity_ - credit_;
    const auto until_full = divide_up(missing, rate_);
    // 先比较填满所需时间，避免长时间闲置时 elapsed * rate 溢出。
    if (static_cast<std::uint64_t>(nanoseconds) >= until_full) {
        credit_ = capacity_;
    } else {
        credit_ += static_cast<std::uint64_t>(nanoseconds) * rate_;
    }
}

PolicyClock::duration TokenBucket::delay(std::size_t cost, PolicyTime now) {
    if (!can_ever_fit(cost)) {
        throw std::invalid_argument("request cannot fit token burst");
    }
    refill(now);
    if (rate_ == 0) {
        return PolicyClock::duration::zero();
    }
    const auto wanted = static_cast<std::uint64_t>(cost) * units_per_token;
    if (wanted <= credit_) {
        return PolicyClock::duration::zero();
    }
    return std::chrono::nanoseconds(divide_up(wanted - credit_, rate_));
}

void TokenBucket::consume(std::size_t cost, PolicyTime now) {
    if (delay(cost, now) != PolicyClock::duration::zero()) {
        throw std::logic_error("tokens must be checked before consumption");
    }
    if (rate_ != 0) {
        credit_ -= static_cast<std::uint64_t>(cost) * units_per_token;
    }
}

const char* timeout_name(TimeoutReason reason) {
    switch (reason) {
    case TimeoutReason::Idle:
        return "idle_timeout";
    case TimeoutReason::Frame:
        return "frame_timeout";
    case TimeoutReason::RateWait:
        return "rate_wait_timeout";
    case TimeoutReason::ResourceWait:
        return "resource_wait_timeout";
    case TimeoutReason::Write:
        return "write_timeout";
    case TimeoutReason::None:
        return "none";
    }
    return "none";
}

ConnectionPolicy::ConnectionPolicy(ConnectionLimits limits, PolicyTime now)
    : limits_(limits),
      messages_(limits.rate.messages_per_second, limits.rate.message_burst, now),
      bytes_(limits.rate.bytes_per_second, limits.rate.byte_burst, now),
      created_(now),
      last_valid_(now) {
    for (const auto limit :
        {limits.timeout.idle, limits.timeout.frame, limits.timeout.wait, limits.timeout.write}) {
        if (limit.count() < 0 || limit > std::chrono::hours(1)) {
            throw std::invalid_argument("connection timeout must be within 0..1 hour");
        }
    }
}

RateDecision ConnectionPolicy::check_rate(std::size_t body_bytes, PolicyTime now) {
    if (!messages_.can_ever_fit(1) || !bytes_.can_ever_fit(body_bytes)) {
        return {false, now};
    }
    const auto wait = std::max(messages_.delay(1, now), bytes_.delay(body_bytes, now));
    return {true, now + wait};
}

void ConnectionPolicy::consume_rate(std::size_t body_bytes, PolicyTime now) {
    const auto checked = check_rate(body_bytes, now);
    if (!checked.possible || checked.retry_at > now) {
        throw std::logic_error("both token budgets must be available");
    }
    // 同一 strand 内、全局帧预算成功后才扣费；反复等待不会重复消费令牌。
    messages_.consume(1, now);
    bytes_.consume(body_bytes, now);
}

void ConnectionPolicy::wait_for(WaitReason reason, PolicyTime now) {
    if (reason == WaitReason::None) {
        throw std::logic_error("wait reason is required");
    }
    if (!wait_at_) {
        wait_at_ = now;
    }
    // 原因切换或重复唤醒都不延长本次等待期限。
    waiting_ = reason;
    if (reason != WaitReason::Rate) {
        rate_retry_.reset();
    }
}

void ConnectionPolicy::wait_for_rate(PolicyTime retry_at, PolicyTime now) {
    wait_for(WaitReason::Rate, now);
    rate_retry_ = retry_at;
}

void ConnectionPolicy::finish_wait() {
    wait_at_.reset();
    rate_retry_.reset();
    waiting_ = WaitReason::None;
}

bool ConnectionPolicy::take_rate_wakeup(PolicyTime now) {
    if (!rate_retry_ || *rate_retry_ > now) {
        return false;
    }
    rate_retry_.reset();
    return true;
}

PeerTime ConnectionPolicy::peer_time(PolicyTime now) const {
    if (paused_at_) {
        return *paused_at_ - created_ - paused_total_;
    }
    return now - created_ - paused_total_;
}

void ConnectionPolicy::pause_peer(bool paused, PolicyTime now) {
    if (paused && !paused_at_) {
        paused_at_ = now;
    } else if (!paused && paused_at_) {
        paused_total_ += now - *paused_at_;
        paused_at_.reset();
    }
}

void ConnectionPolicy::frame_started(PeerTime first_byte) {
    if (!frame_at_) {
        frame_at_ = first_byte;
    }
}

void ConnectionPolicy::frame_completed() {
    frame_at_.reset();
}

void ConnectionPolicy::valid_request(PolicyTime received_at) {
    last_valid_ = received_at;
}

void ConnectionPolicy::write_started(PolicyTime now) {
    write_at_ = now;
}

void ConnectionPolicy::write_finished() {
    write_at_.reset();
}

TimeoutReason ConnectionPolicy::expired(PolicyTime now) const {
    if (write_at_ && elapsed(*write_at_, limits_.timeout.write, now)) {
        return TimeoutReason::Write;
    }
    if (wait_at_ && elapsed(*wait_at_, limits_.timeout.wait, now)) {
        if (waiting_ == WaitReason::Rate) {
            return TimeoutReason::RateWait;
        }
        return TimeoutReason::ResourceWait;
    }
    if (!paused_at_) {
        const auto peer = peer_time(now);
        if (frame_at_ && limits_.timeout.frame.count() > 0 &&
            peer - *frame_at_ >= limits_.timeout.frame) {
            return TimeoutReason::Frame;
        }
    }
    if (elapsed(last_valid_, limits_.timeout.idle, now)) {
        return TimeoutReason::Idle;
    }
    return TimeoutReason::None;
}

std::optional<PolicyTime> ConnectionPolicy::next_deadline(PolicyTime now) const {
    std::optional<PolicyTime> next;
    const auto include = [&next](PolicyTime candidate) {
        if (!next || candidate < *next) {
            next = candidate;
        }
    };
    if (limits_.timeout.idle.count() > 0) {
        include(last_valid_ + limits_.timeout.idle);
    }
    if (!paused_at_) {
        const auto peer = peer_time(now);
        if (frame_at_ && limits_.timeout.frame.count() > 0) {
            include(now + limits_.timeout.frame - (peer - *frame_at_));
        }
    }
    if (wait_at_ && limits_.timeout.wait.count() > 0) {
        include(*wait_at_ + limits_.timeout.wait);
    }
    if (write_at_ && limits_.timeout.write.count() > 0) {
        include(*write_at_ + limits_.timeout.write);
    }
    if (rate_retry_) {
        include(*rate_retry_);
    }
    return next;
}

} // namespace gateway
