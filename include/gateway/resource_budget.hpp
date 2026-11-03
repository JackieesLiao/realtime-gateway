#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace gateway {

inline constexpr std::size_t read_buffer_bytes = 4096;
inline constexpr std::size_t max_ack_frame_bytes = 256;
inline constexpr std::size_t request_reserve_padding = 16;

enum class Resource { Read, Receive, Business, Send };

struct ResourceUsage {
    std::size_t messages = 0;
    std::size_t bytes = 0;
};

struct ResourceBudgetLimits {
    std::size_t connections = 1024;
    std::size_t receive_body_bytes = 1048576;
    ResourceUsage receive{256, 64 * 1024 * 1024};
    ResourceUsage business{256, 16 * 1024 * 1024};
    ResourceUsage local_send{64, 262144};
    ResourceUsage send{4096, 16 * 1024 * 1024};
    ResourceUsage total{8192, 128 * 1024 * 1024};
    unsigned high_percent = 80;
    unsigned low_percent = 50;
};

struct ResourceBudgetState;
struct BudgetAccount;

// M2.3：唯一、可移动的资源凭证。先销毁数据，再释放持有它的凭证。
// 状态由 shared_ptr 保持，连接关闭后在途回调仍能归还全局预算。
class ResourceLease {
public:
    ResourceLease() = default;
    ~ResourceLease();
    ResourceLease(const ResourceLease&) = delete;
    ResourceLease& operator=(const ResourceLease&) = delete;
    ResourceLease(ResourceLease&& other) noexcept;
    ResourceLease& operator=(ResourceLease&& other) noexcept;

    explicit operator bool() const;
    std::size_t bytes() const;
    void shrink_bytes(std::size_t bytes);
    void release();

private:
    friend class ResourceBudget;
    std::shared_ptr<ResourceBudgetState> state_;
    std::shared_ptr<BudgetAccount> account_;
    Resource resource_ = Resource::Read;
    ResourceUsage amount_;
};

struct TaskResources {
    ResourceLease business;
    ResourceLease reply;
};

struct FrameReservation {
    ResourceLease receive;
    TaskResources task;
};

// 一个已投递的唤醒也有唯一凭证：析构时清除 pending 标志并推进下一位等待者。
class WakeupTicket {
public:
    WakeupTicket() = default;
    ~WakeupTicket();
    WakeupTicket(const WakeupTicket&) = delete;
    WakeupTicket& operator=(const WakeupTicket&) = delete;
    WakeupTicket(WakeupTicket&& other) noexcept;
    WakeupTicket& operator=(WakeupTicket&& other) noexcept;

private:
    friend struct ResourceBudgetState;
    void finish();
    std::shared_ptr<ResourceBudgetState> state_;
    std::shared_ptr<BudgetAccount> account_;
};

enum class BudgetStatus { Acquired, Waiting, Impossible };

enum class BudgetFailure { None, Receive, Business, Send, Total };

struct FrameAdmission {
    BudgetStatus status = BudgetStatus::Waiting;
    BudgetFailure failure = BudgetFailure::None;
    FrameReservation reservation;
};

struct ConnectionBudget {
    std::shared_ptr<BudgetAccount> account;
    ResourceLease read;
};

struct QueueTurn {
    bool allowed;
    std::uint64_t epoch;
};

struct ResourceBudgetSnapshot {
    std::array<ResourceUsage, 4> used{};
    std::array<ResourceUsage, 4> peak{};
    ResourceUsage total;
    ResourceUsage peak_total;
    std::size_t budget_waiters = 0;
    std::size_t queue_waiters = 0;
    std::size_t pending_wakeups = 0;
};

class ResourceBudget {
public:
    using Wakeup = std::function<void(WakeupTicket)>;

    explicit ResourceBudget(ResourceBudgetLimits limits);
    std::optional<ConnectionBudget> open(std::uint64_t id);
    void set_wakeup(const std::shared_ptr<BudgetAccount>& account, Wakeup wakeup);
    void close(const std::shared_ptr<BudgetAccount>& account);

    // 包头已校验后，全有或全无地预留 RX、业务副本和最坏 ACK 空间。
    FrameAdmission try_frame(const std::shared_ptr<BudgetAccount>& account, std::size_t body_bytes);
    bool allow_read(const std::shared_ptr<BudgetAccount>& account);

    // 队列有自己的锁，epoch 解决 try_submit 失败与登记等待之间的释放竞态。
    QueueTurn queue_turn(const std::shared_ptr<BudgetAccount>& account);
    void wait_queue(const std::shared_ptr<BudgetAccount>& account, std::uint64_t observed_epoch);
    void queue_accepted(const std::shared_ptr<BudgetAccount>& account);
    void notify_task_capacity() const;

    ResourceBudgetSnapshot snapshot() const;

private:
    std::shared_ptr<ResourceBudgetState> state_;
};

} // namespace gateway
