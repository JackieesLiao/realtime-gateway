#pragma once

#include "gateway/bounded_task_queue.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

namespace gateway {

class Dispatcher {
public:
    using Handler = std::function<realtime::Ack(const realtime::Request&)>;

    // Handler 必须支持多个 worker 并发调用，不依赖网络类型；Router 使用原子统计。
    Dispatcher(std::size_t worker_threads, TaskQueueLimits limits, Handler handler);
    ~Dispatcher();

    Dispatcher(const Dispatcher&) = delete;
    Dispatcher& operator=(const Dispatcher&) = delete;

    SubmitStatus try_submit(std::unique_ptr<realtime::Request>& request,
        BusinessCompletion completion,
        TaskResources* resources = nullptr);
    void stop();
    void set_capacity_notifier(std::function<void()> notifier);

    // 只在 I/O run() 返回后的控制线程调用，不能在 I/O 回调中等待 worker。
    void join();
    TaskQueueSnapshot snapshot() const;
    std::uint64_t failed_tasks() const;
    std::uint64_t failed_completions() const;

private:
    void run_worker();

    BoundedTaskQueue queue_;
    Handler handler_;
    std::atomic<std::uint64_t> failed_tasks_{0};
    std::atomic<std::uint64_t> failed_completions_{0};

    // 最后声明，构造失败时先 join 已启动的线程，再销毁它们依赖的成员。
    std::vector<std::jthread> workers_;
};

} // namespace gateway
