#include "gateway/dispatcher.hpp"

#include <stdexcept>
#include <utility>

namespace gateway {

Dispatcher::Dispatcher(std::size_t worker_threads, TaskQueueLimits limits, Handler handler)
    : queue_(limits), handler_(std::move(handler)) {
    if (worker_threads == 0 || !handler_) {
        throw std::invalid_argument("workers and business handler are required");
    }

    workers_.reserve(worker_threads);
    try {
        for (std::size_t index = 0; index < worker_threads; ++index) {
            workers_.emplace_back([this] {
                run_worker();
            });
        }
    } catch (...) {
        queue_.stop();
        throw;
    }
}

Dispatcher::~Dispatcher() {
    stop();
    join();
}

SubmitStatus Dispatcher::try_submit(std::unique_ptr<realtime::Request>& request,
    BusinessCompletion completion,
    TaskResources* resources) {
    return queue_.try_push(request, std::move(completion), resources);
}

void Dispatcher::run_worker() {
    while (auto task = queue_.wait_pop()) {
        BusinessResult result;
        const auto request_id = task->request->request_id();
        try {
            // wait_pop 已经释放队列锁；耗时业务绝不能放在队列临界区内。
            result.ack = handler_(*task->request);
        } catch (...) {
            failed_tasks_.fetch_add(1, std::memory_order_relaxed);
            result.ack.set_result(realtime::INTERNAL_ERROR);
            result.ack.set_error("business handler failed");
        }
        result.ack.set_request_id(request_id);
        if (result.ack.ByteSizeLong() == 0 || result.ack.ByteSizeLong() > max_ack_frame_bytes - 4) {
            // Handler 的输出也有硬边界，不能把无界错误信息转移到完成回调队列。
            {
                realtime::Ack discarded;
                discarded.Swap(&result.ack);
            }
            failed_tasks_.fetch_add(1, std::memory_order_relaxed);
            result.ack.set_request_id(request_id);
            result.ack.set_result(realtime::INTERNAL_ERROR);
            result.ack.set_error("business result exceeds ACK limit");
        }

        auto completion = std::move(task->completion);
        result.ticket = std::move(task->ticket);
        result.reply_budget = result.ticket.take_reply_budget();
        task.reset();
        result.ticket.finish_execution();

        try {
            // Request 已销毁，但总在途预算仍由 result 持有，直到回送被消费。
            completion(std::move(result));
        } catch (...) {
            // 完成回调抛错也不能杀死 worker；栈展开会归还未转移的凭证。
            failed_completions_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void Dispatcher::set_capacity_notifier(std::function<void()> notifier) {
    queue_.set_capacity_notifier(std::move(notifier));
}

void Dispatcher::stop() {
    queue_.stop();
}

void Dispatcher::join() {
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

TaskQueueSnapshot Dispatcher::snapshot() const {
    return queue_.snapshot();
}

std::uint64_t Dispatcher::failed_tasks() const {
    return failed_tasks_.load(std::memory_order_relaxed);
}

std::uint64_t Dispatcher::failed_completions() const {
    return failed_completions_.load(std::memory_order_relaxed);
}

} // namespace gateway
