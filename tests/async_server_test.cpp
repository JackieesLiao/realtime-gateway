#include "gateway/frame.hpp"
#include "gateway/server.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <gtest/gtest.h>

using namespace gateway;
using namespace std::chrono_literals;
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

namespace {

std::string request_frame(std::uint64_t id) {
    realtime::Request request;
    request.set_request_id(id);
    request.set_device_id("robot");
    request.set_message_type(realtime::TELEMETRY);
    request.set_payload("abc");
    return encode_frame(request.SerializeAsString());
}

// 测试客户端使用带超时的原生 send/recv，避免失败时阻塞整个测试进程。
class Client {
public:
    explicit Client(std::uint16_t port) : socket_(io_) {
        socket_.connect({asio::ip::make_address("127.0.0.1"), port});
        const timeval timeout{3, 0};
        if (::setsockopt(
                socket_.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
            ::setsockopt(
                socket_.native_handle(), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
            throw std::system_error(errno, std::generic_category(), "setsockopt");
        }
    }

    void send(const std::string& bytes) {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto sent = ::send(socket_.native_handle(),
                bytes.data() + offset,
                bytes.size() - offset,
                MSG_NOSIGNAL);
            if (sent <= 0) {
                throw std::system_error(errno, std::generic_category(), "send");
            }
            offset += static_cast<std::size_t>(sent);
        }
    }

    realtime::Ack receive_ack() {
        const auto header = receive_exact(4);
        std::uint32_t size = 0;
        for (const unsigned char byte : header) {
            size = (size << 8) | byte;
        }
        if (size == 0 || size > 1048576) {
            throw std::runtime_error("invalid ACK size");
        }

        realtime::Ack ack;
        if (!ack.ParseFromString(receive_exact(size))) {
            throw std::runtime_error("invalid ACK protobuf");
        }
        return ack;
    }

    bool has_no_ack() {
        char byte = 0;
        const auto received = ::recv(socket_.native_handle(), &byte, 1, MSG_PEEK | MSG_DONTWAIT);
        return received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
    }

    bool is_closed() {
        char byte = 0;
        const auto received = ::recv(socket_.native_handle(), &byte, 1, 0);
        return received == 0 || (received < 0 && errno == ECONNRESET);
    }

private:
    std::string receive_exact(std::size_t size) {
        std::string bytes(size, '\0');
        std::size_t offset = 0;
        while (offset < size) {
            const auto received =
                ::recv(socket_.native_handle(), bytes.data() + offset, size - offset, 0);
            if (received <= 0) {
                throw std::runtime_error("ACK receive failed or timed out");
            }
            offset += static_cast<std::size_t>(received);
        }
        return bytes;
    }

    asio::io_context io_;
    tcp::socket socket_;
};

class BusinessGate {
public:
    realtime::Ack execute(const realtime::Request& request) {
        std::unique_lock lock(mutex_);
        entered_.push_back(request.request_id());
        changed_.notify_all();
        if (request.request_id() == 1) {
            changed_.wait(lock, [this] {
                return open_;
            });
        }

        realtime::Ack ack;
        ack.set_result(realtime::OK);
        return ack;
    }

    bool wait_first() {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, 3s, [this] {
            return !entered_.empty();
        });
    }

    std::vector<std::uint64_t> entered() {
        std::lock_guard lock(mutex_);
        return entered_;
    }

    void open() {
        {
            std::lock_guard lock(mutex_);
            open_ = true;
        }
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<std::uint64_t> entered_;
    bool open_ = false;
};

class RunningServer {
public:
    RunningServer(
        Config config, Dispatcher::Handler handler, std::shared_ptr<BusinessGate> gate = {})
        : gate_(std::move(gate)),
          server_(std::make_shared<Server>(io_, config, std::move(handler))) {
        server_->start();
        io_thread_ = std::jthread([this] {
            try {
                io_.run();
            } catch (...) {
                io_.stop();
            }
        });
    }

    ~RunningServer() {
        finish();
    }

    std::uint16_t port() const {
        return server_->port();
    }

    void synchronize_io() {
        auto reached = std::make_shared<std::promise<void>>();
        auto future = reached->get_future();
        asio::post(io_, [reached] {
            reached->set_value();
        });
        if (future.wait_for(3s) != std::future_status::ready) {
            throw std::runtime_error("I/O thread blocked");
        }
    }

    // 真实 timer 桥接测试使用明确的计时里程碑；不用于猜测业务队列或 TCP 分包。
    void wait_for_io_timer(std::chrono::milliseconds duration) {
        auto timer = std::make_shared<asio::steady_timer>(io_, duration);
        auto reached = std::make_shared<std::promise<void>>();
        auto future = reached->get_future();
        timer->async_wait([timer, reached](const auto& error) {
            if (!error) {
                reached->set_value();
            }
        });
        if (future.wait_for(3s) != std::future_status::ready) {
            throw std::runtime_error("test timer did not complete");
        }
    }

    void stop() {
        server_->stop();
    }

    void finish() {
        if (finished_) {
            return;
        }
        if (gate_) {
            gate_->open();
        }
        server_->stop();
        server_->join_workers();
        if (io_thread_.joinable()) {
            io_thread_.join();
        }
        finished_ = true;
        const auto budget = server_->budget_snapshot();
        EXPECT_EQ(budget.total.messages, 0U);
        EXPECT_EQ(budget.total.bytes, 0U);
        EXPECT_EQ(budget.budget_waiters + budget.queue_waiters, 0U);
        EXPECT_EQ(budget.pending_wakeups, 0U);
    }

    TaskQueueSnapshot snapshot() const {
        return server_->business_snapshot();
    }

    ResourceBudgetSnapshot budget_snapshot() const {
        return server_->budget_snapshot();
    }

    // 观察生产状态并通过 I/O barrier 让事件推进，不靠 sleep 猜测分包或队列时序。
    bool wait_for_budget(const std::function<bool(const ResourceBudgetSnapshot&)>& reached) {
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (std::chrono::steady_clock::now() < deadline) {
            if (reached(budget_snapshot())) {
                return true;
            }
            synchronize_io();
        }
        return false;
    }

private:
    std::shared_ptr<BusinessGate> gate_;
    asio::io_context io_;
    std::shared_ptr<Server> server_;
    std::jthread io_thread_;
    bool finished_ = false;
};

Config test_config() {
    Config config;
    config.port = 0;
    config.io_threads = 1;
    return config;
}

} // namespace

TEST(AsyncServer, NoAckBeforeBusinessCompletesAndBufferedRequestsKeepOrder) {
    auto gate = std::make_shared<BusinessGate>();
    RunningServer server(
        test_config(),
        [gate](const auto& request) {
            return gate->execute(request);
        },
        gate);
    Client client(server.port());
    const auto third = request_frame(3);
    client.send(request_frame(1) + request_frame(2) + third.substr(0, 6));
    ASSERT_TRUE(gate->wait_first());
    server.synchronize_io();
    EXPECT_TRUE(client.has_no_ack());
    EXPECT_EQ(gate->entered(), (std::vector<std::uint64_t>{1}));

    gate->open();
    EXPECT_EQ(client.receive_ack().request_id(), 1U);
    EXPECT_EQ(client.receive_ack().request_id(), 2U);
    client.send(third.substr(6));
    EXPECT_EQ(client.receive_ack().request_id(), 3U);
    EXPECT_EQ(gate->entered(), (std::vector<std::uint64_t>{1, 2, 3}));
}

TEST(AsyncServer, BlockedBusinessDoesNotBlockOtherConnectionsOrIo) {
    auto gate = std::make_shared<BusinessGate>();
    RunningServer server(
        test_config(),
        [gate](const auto& request) {
            return gate->execute(request);
        },
        gate);
    Client slow(server.port());
    slow.send(request_frame(1));
    ASSERT_TRUE(gate->wait_first());

    Client fast(server.port());
    fast.send(request_frame(2));
    const auto ack = fast.receive_ack();
    EXPECT_EQ(ack.request_id(), 2U);
    EXPECT_EQ(ack.result(), realtime::OK);
    EXPECT_TRUE(slow.has_no_ack());
    gate->open();
    EXPECT_EQ(slow.receive_ack().request_id(), 1U);
}

TEST(AsyncServer, OutstandingLimitOneAllowsNextBufferedRequestAfterCompletion) {
    auto config = test_config();
    config.business_queue.outstanding_messages = 1;
    RunningServer server(config, [](const auto&) {
        realtime::Ack ack;
        ack.set_result(realtime::OK);
        return ack;
    });
    Client client(server.port());
    client.send(request_frame(1) + request_frame(2) + request_frame(3));
    for (std::uint64_t id = 1; id <= 3; ++id) {
        const auto ack = client.receive_ack();
        EXPECT_EQ(ack.request_id(), id);
        EXPECT_EQ(ack.result(), realtime::OK);
    }
}

TEST(AsyncServer, BusinessExceptionReturnsErrorAndKeepsConnectionUsable) {
    RunningServer server(test_config(), [](const auto& request) {
        if (request.request_id() == 1) {
            throw std::runtime_error("controlled failure");
        }
        realtime::Ack ack;
        ack.set_result(realtime::OK);
        return ack;
    });
    Client client(server.port());
    client.send(request_frame(1) + request_frame(2));
    const auto failed = client.receive_ack();
    EXPECT_EQ(failed.request_id(), 1U);
    EXPECT_EQ(failed.result(), realtime::INTERNAL_ERROR);
    EXPECT_FALSE(failed.error().empty());
    const auto succeeded = client.receive_ack();
    EXPECT_EQ(succeeded.request_id(), 2U);
    EXPECT_EQ(succeeded.result(), realtime::OK);
}

TEST(AsyncServer, CloseBeforeCompletionDropsLateResultAndReturnsBudget) {
    auto gate = std::make_shared<BusinessGate>();
    RunningServer server(
        test_config(),
        [gate](const auto& request) {
            return gate->execute(request);
        },
        gate);
    Client client(server.port());
    client.send(request_frame(1));
    ASSERT_TRUE(gate->wait_first());
    server.stop();
    ASSERT_TRUE(client.is_closed());
    EXPECT_EQ(server.snapshot().outstanding_messages, 1U);

    gate->open();
    server.finish();
    EXPECT_EQ(server.snapshot().outstanding_messages, 0U);
    EXPECT_EQ(server.snapshot().outstanding_bytes, 0U);
    EXPECT_EQ(server.snapshot().executing_messages, 0U);
}

TEST(AsyncServer, QueueFullPausesAndRecoversWithoutDroppingAcceptedConnections) {
    auto config = test_config();
    config.business_threads = 1;
    config.business_queue.queued_messages = 1;
    auto gate = std::make_shared<BusinessGate>();
    RunningServer server(
        config,
        [gate](const auto& request) {
            return gate->execute(request);
        },
        gate);
    Client first(server.port());
    first.send(request_frame(1));
    ASSERT_TRUE(gate->wait_first());
    Client second(server.port());
    Client third(server.port());
    second.send(request_frame(2));
    third.send(request_frame(3));
    ASSERT_TRUE(server.wait_for_budget([](const auto& budget) {
        return budget.queue_waiters == 1;
    }));
    EXPECT_EQ(server.snapshot().queued_messages, 1U);
    EXPECT_TRUE(second.has_no_ack());
    EXPECT_TRUE(third.has_no_ack());
    gate->open();
    EXPECT_EQ(first.receive_ack().request_id(), 1U);
    EXPECT_EQ(second.receive_ack().request_id(), 2U);
    EXPECT_EQ(third.receive_ack().request_id(), 3U);
}

TEST(AsyncServer, GlobalWaiterProgressesBeforeOwnerCanReacquireSinglePipeline) {
    auto config = test_config();
    config.global_budget.messages = 3;
    auto gate = std::make_shared<BusinessGate>();
    RunningServer server(
        config,
        [gate](const auto& request) {
            return gate->execute(request);
        },
        gate);
    Client first(server.port());
    Client second(server.port());
    ASSERT_TRUE(server.wait_for_budget([](const auto& budget) {
        return budget.used[0].bytes == 2 * read_buffer_bytes;
    }));
    server.synchronize_io();
    server.synchronize_io();
    first.send(request_frame(1));
    ASSERT_TRUE(gate->wait_first());
    second.send(request_frame(2));
    ASSERT_TRUE(server.wait_for_budget([](const auto& budget) {
        return budget.budget_waiters >= 1;
    }));
    first.send(request_frame(4));
    EXPECT_TRUE(second.has_no_ack());
    gate->open();
    EXPECT_EQ(first.receive_ack().request_id(), 1U);
    EXPECT_EQ(second.receive_ack().request_id(), 2U);
    EXPECT_EQ(first.receive_ack().request_id(), 4U);
    EXPECT_EQ(gate->entered(), (std::vector<std::uint64_t>{1, 2, 4}));
    EXPECT_LE(server.budget_snapshot().peak_total.messages, 3U);
}

TEST(AsyncServer, AdmittedPartialFrameContinuesDespiteReceiveHighWater) {
    auto config = test_config();
    config.global_receive.messages = 1;
    auto gate = std::make_shared<BusinessGate>();
    gate->open();
    RunningServer server(
        config,
        [gate](const auto& request) {
            return gate->execute(request);
        },
        gate);
    Client first(server.port());
    Client second(server.port());
    const auto wire = request_frame(1);
    first.send(wire.substr(0, 6));
    ASSERT_TRUE(server.wait_for_budget([](const auto& budget) {
        return budget.used[1].messages == 1;
    }));
    second.send(request_frame(2));
    ASSERT_TRUE(server.wait_for_budget([](const auto& budget) {
        return budget.budget_waiters >= 1;
    }));
    first.send(wire.substr(6));
    EXPECT_EQ(first.receive_ack().request_id(), 1U);
    EXPECT_EQ(second.receive_ack().request_id(), 2U);
    EXPECT_LE(server.budget_snapshot().peak[1].messages, 1U);
}

TEST(AsyncServer, OneAckSlotWakesBufferedRequestsAfterWriteCompletion) {
    auto config = test_config();
    config.max_send_messages = 1;
    config.max_send_bytes = max_ack_frame_bytes;
    RunningServer server(config, [](const auto&) {
        realtime::Ack ack;
        ack.set_result(realtime::OK);
        return ack;
    });
    Client client(server.port());
    client.send(request_frame(1) + request_frame(2) + request_frame(3));
    for (std::uint64_t id = 1; id <= 3; ++id) {
        EXPECT_EQ(client.receive_ack().request_id(), id);
    }
    EXPECT_EQ(server.budget_snapshot().peak[3].messages, 1U);
    EXPECT_LE(server.budget_snapshot().peak[3].bytes, max_ack_frame_bytes);
}

TEST(AsyncServer, ClosingWithPartialAndWaitingFramesReturnsEveryReservation) {
    auto config = test_config();
    config.global_receive.messages = 1;
    RunningServer server(config, [](const auto&) {
        realtime::Ack ack;
        return ack;
    });
    Client first(server.port());
    Client second(server.port());
    first.send(request_frame(1).substr(0, 6));
    ASSERT_TRUE(server.wait_for_budget([](const auto& budget) {
        return budget.used[1].messages == 1;
    }));
    second.send(request_frame(2));
    ASSERT_TRUE(server.wait_for_budget([](const auto& budget) {
        return budget.budget_waiters >= 1;
    }));
    server.stop();
    EXPECT_TRUE(first.is_closed());
    EXPECT_TRUE(second.is_closed());
    server.finish();
    EXPECT_EQ(server.budget_snapshot().total.bytes, 0U);
    EXPECT_EQ(server.budget_snapshot().pending_wakeups, 0U);
}

TEST(AsyncServer, OversizedHandlerAckBecomesBoundedFailure) {
    RunningServer server(test_config(), [](const auto&) {
        realtime::Ack ack;
        ack.set_error(std::string(max_ack_frame_bytes, 'x'));
        return ack;
    });
    Client client(server.port());
    client.send(request_frame(2));
    const auto ack = client.receive_ack();
    EXPECT_EQ(ack.request_id(), 2U);
    EXPECT_EQ(ack.result(), realtime::INTERNAL_ERROR);
    EXPECT_EQ(ack.error(), "business result exceeds ACK limit");
    EXPECT_LE(ack.ByteSizeLong() + 4, max_ack_frame_bytes);
    EXPECT_LE(server.budget_snapshot().peak[3].bytes, max_ack_frame_bytes);
}

TEST(AsyncServer, StopWithExecutingQueuedAndCapacityWaitingTasksReturnsAllBudgets) {
    auto config = test_config();
    config.business_threads = 1;
    config.business_queue.queued_messages = 1;
    auto gate = std::make_shared<BusinessGate>();
    RunningServer server(
        config,
        [gate](const auto& request) {
            return gate->execute(request);
        },
        gate);
    Client first(server.port());
    first.send(request_frame(1));
    ASSERT_TRUE(gate->wait_first());
    Client second(server.port());
    Client third(server.port());
    second.send(request_frame(2));
    third.send(request_frame(3));
    ASSERT_TRUE(server.wait_for_budget([](const auto& budget) {
        return budget.queue_waiters == 1;
    }));
    server.stop();
    EXPECT_TRUE(first.is_closed());
    EXPECT_TRUE(second.is_closed());
    EXPECT_TRUE(third.is_closed());
    ASSERT_TRUE(server.wait_for_budget([](const auto& budget) {
        return budget.used[2].messages == 1 && budget.queue_waiters == 0;
    }));
    EXPECT_EQ(server.snapshot().queued_messages, 0U);
    EXPECT_EQ(server.snapshot().outstanding_messages, 1U);
    gate->open();
    server.finish();
    EXPECT_EQ(gate->entered(), (std::vector<std::uint64_t>{1}));
    EXPECT_EQ(server.budget_snapshot().total.bytes, 0U);
}

TEST(AsyncServer, ResourceWaitDeadlineClosesOnlyWaiterAndRefundsBudget) {
    auto config = test_config();
    config.global_receive.messages = 1;
    config.connection.timeout = {0ms, 0ms, 150ms, 0ms};
    RunningServer server(config, [](const auto&) {
        realtime::Ack ack;
        return ack;
    });
    Client first(server.port());
    const auto wire = request_frame(1);
    first.send(wire.substr(0, 6));
    ASSERT_TRUE(server.wait_for_budget([](const auto& budget) {
        return budget.used[1].messages == 1;
    }));
    Client second(server.port());
    second.send(request_frame(2));
    EXPECT_TRUE(second.is_closed());
    first.send(wire.substr(6));
    EXPECT_EQ(first.receive_ack().request_id(), 1U);
    server.finish();
    EXPECT_EQ(server.budget_snapshot().total.bytes, 0U);
}

TEST(AsyncServer, IdleTimeoutDuringBusinessDiscardsLateResultSafely) {
    auto config = test_config();
    config.connection.timeout = {150ms, 0ms, 0ms, 0ms};
    auto gate = std::make_shared<BusinessGate>();
    RunningServer server(
        config,
        [gate](const auto& request) {
            return gate->execute(request);
        },
        gate);
    Client client(server.port());
    client.send(request_frame(1));
    ASSERT_TRUE(gate->wait_first());
    EXPECT_TRUE(client.is_closed());
    EXPECT_EQ(server.snapshot().outstanding_messages, 1U);
    gate->open();
    server.finish();
    EXPECT_EQ(server.snapshot().outstanding_messages, 0U);
}

TEST(AsyncServer, RefreshedIdleDeadlineSurvivesOldTimerWakeup) {
    auto config = test_config();
    config.connection.timeout = {1000ms, 0ms, 0ms, 0ms};
    RunningServer server(config, [](const auto&) {
        realtime::Ack ack;
        return ack;
    });
    Client client(server.port());
    client.send(request_frame(1));
    EXPECT_EQ(client.receive_ack().request_id(), 1U);
    server.wait_for_io_timer(400ms);
    client.send(request_frame(2));
    EXPECT_EQ(client.receive_ack().request_id(), 2U);
    // 超过原 1 秒期限，仍在第二次有效消息的新期限内。
    server.wait_for_io_timer(700ms);
    client.send(request_frame(3));
    EXPECT_EQ(client.receive_ack().request_id(), 3U);
}
