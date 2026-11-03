#pragma once

#include "gateway.pb.h"
#include "gateway/resource_budget.hpp"

#include <cstddef>
#include <functional>
#include <memory>

namespace gateway {

struct TaskQueueLimits {
    std::size_t queued_messages = 128;
    std::size_t queued_bytes = 8 * 1024 * 1024;
    std::size_t outstanding_messages = 256;
    std::size_t outstanding_bytes = 16 * 1024 * 1024;
    unsigned high_percent = 80;
    unsigned low_percent = 50;
};

enum class SubmitStatus {
    Accepted, // 已经接管请求和完成回调。
    Full,     // 任一数量/字节限制不足，请求仍属于调用方。
    Stopping  // 停止后不再接受新任务。
};

struct TaskQueueSnapshot {
    std::size_t queued_messages;
    std::size_t queued_bytes;
    std::size_t executing_messages;
    std::size_t outstanding_messages;
    std::size_t outstanding_bytes;
    bool stopping;
    bool paused;
};

struct TaskQueueState;

// M2.2：唯一、可移动的任务预算凭证；结果被消费或丢弃后才归还总在途预算。
// 凭证持有共享计数状态，因此可以安全地比队列/Dispatcher 活得更久。
class TaskTicket {
public:
    TaskTicket() = default;
    ~TaskTicket();

    TaskTicket(const TaskTicket&) = delete;
    TaskTicket& operator=(const TaskTicket&) = delete;
    TaskTicket(TaskTicket&& other) noexcept;
    TaskTicket& operator=(TaskTicket&& other) noexcept;

    void finish_execution();
    void release();
    ResourceLease take_reply_budget();

private:
    friend class BoundedTaskQueue;

    std::shared_ptr<TaskQueueState> state_;
    std::size_t bytes_ = 0;
    bool executing_ = false;
    TaskResources resources_;
};

struct BusinessResult {
    // 成员逆序销毁：先销毁 ACK，最后才归还任务预算。
    TaskTicket ticket;
    ResourceLease reply_budget;
    realtime::Ack ack;
};

using BusinessCompletion = std::function<void(BusinessResult)>;

struct BusinessTask {
    // 停止时丢弃排队任务，也必须先释放 Request/回调，再归还预算。
    TaskTicket ticket;
    std::unique_ptr<realtime::Request> request;
    BusinessCompletion completion;
};

class BoundedTaskQueue {
public:
    explicit BoundedTaskQueue(TaskQueueLimits limits);
    ~BoundedTaskQueue();

    BoundedTaskQueue(const BoundedTaskQueue&) = delete;
    BoundedTaskQueue& operator=(const BoundedTaskQueue&) = delete;

    // 不等待空位。只有 Accepted 才移动 request，失败时调用方仍持有原请求。
    SubmitStatus try_push(std::unique_ptr<realtime::Request>& request,
        BusinessCompletion completion,
        TaskResources* resources = nullptr);

    // 仅供业务线程调用：队列为空时睡眠，stop 后返回空指针。
    std::unique_ptr<BusinessTask> wait_pop();
    void stop();
    TaskQueueSnapshot snapshot() const;
    void set_capacity_notifier(std::function<void()> notifier);

private:
    std::shared_ptr<TaskQueueState> state_;
};

} // namespace gateway
