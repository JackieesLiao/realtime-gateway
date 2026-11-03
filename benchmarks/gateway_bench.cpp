#include "gateway.pb.h"
#include "gateway/frame.hpp"

#include <utility>
#include <boost/asio.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <deque>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <poll.h>

namespace {

using Clock = std::chrono::steady_clock;
using Nanoseconds = std::chrono::nanoseconds;
using tcp = boost::asio::ip::tcp;

struct Options {
    std::uint16_t port;
    std::size_t connections;
    std::size_t window;
    std::size_t payload_bytes;
    unsigned warmup_seconds;
    unsigned measure_seconds;
    std::string samples_path;
    std::size_t max_samples;
};

struct Sample {
    std::uint64_t request_id;
    std::int64_t sent_ns;
    std::int64_t ack_ns;
};

struct Pending {
    std::uint64_t request_id;
    Clock::time_point sent;
};

struct ConnectionResult {
    std::vector<Sample> samples;
    std::uint64_t sent = 0;
    std::uint64_t acknowledged = 0;
    std::uint64_t acknowledged_in_interval = 0;
    std::uint64_t payload_bytes = 0;
    std::string error;
};

std::size_t number(const char* text, std::size_t minimum, std::size_t maximum) {
    const std::string input(text);
    std::size_t end = 0;
    const auto value = std::stoull(input, &end);
    if (end != input.size() || input.front() == '-' || value < minimum || value > maximum) {
        throw std::invalid_argument("numeric argument outside supported range: " + input);
    }
    return static_cast<std::size_t>(value);
}

Options parse_options(int argc, char** argv) {
    if (argc != 9) {
        throw std::invalid_argument("usage: gateway_bench PORT CONNECTIONS WINDOW PAYLOAD_BYTES "
                                    "WARMUP_SECONDS MEASURE_SECONDS SAMPLES.csv MAX_SAMPLES");
    }
    return {static_cast<std::uint16_t>(number(argv[1], 1, 65535)),
        number(argv[2], 1, 128),
        number(argv[3], 1, 256),
        number(argv[4], 1, 65536),
        static_cast<unsigned>(number(argv[5], 0, 30)),
        static_cast<unsigned>(number(argv[6], 1, 60)),
        argv[7],
        number(argv[8], 128, 5000000)};
}

std::int64_t relative_ns(Clock::time_point value, Clock::time_point origin) {
    return std::chrono::duration_cast<Nanoseconds>(value - origin).count();
}

// 客户端只对 loopback 使用；每次收发有独立期限，失败会保留错误并使进程非零退出。
void transfer(tcp::socket& socket, char* data, std::size_t size, bool sending) {
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    std::size_t offset = 0;
    while (offset < size) {
        boost::system::error_code error;
        const auto remaining = boost::asio::buffer(data + offset, size - offset);
        const auto bytes =
            sending ? socket.write_some(remaining, error) : socket.read_some(remaining, error);
        if (!error) {
            offset += bytes;
            continue;
        }
        if (error != boost::asio::error::would_block && error != boost::asio::error::try_again) {
            throw boost::system::system_error(error);
        }
        const auto milliseconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (milliseconds <= 0) {
            throw std::runtime_error("benchmark socket deadline exceeded");
        }
        pollfd descriptor{
            socket.native_handle(), static_cast<short>(sending ? POLLOUT : POLLIN), 0};
        if (::poll(&descriptor, 1, static_cast<int>(milliseconds)) < 0 && errno != EINTR) {
            throw std::runtime_error("benchmark poll failed");
        }
    }
}

void run_connection(tcp::socket& socket,
    const Options& options,
    std::size_t connection,
    Clock::time_point start,
    ConnectionResult& result) {
    const auto measure_start = start + std::chrono::seconds(options.warmup_seconds);
    const auto measure_end = measure_start + std::chrono::seconds(options.measure_seconds);
    const auto sample_capacity = options.max_samples / options.connections;
    result.samples.reserve(sample_capacity);
    std::deque<Pending> pending;
    realtime::Request request;
    request.set_device_id("benchmark-device");
    request.set_message_type(realtime::TELEMETRY);
    request.set_payload(std::string(options.payload_bytes, 'x'));
    std::uint64_t sequence = 0;

    const auto send_one = [&] {
        // 高位区分连接，低位是单调序号；ACK 必须匹配当前连接队首请求。
        const auto id = ((static_cast<std::uint64_t>(connection) + 1) << 48) | ++sequence;
        request.set_request_id(id);
        auto wire = gateway::encode_frame(request.SerializeAsString());
        const auto sent = Clock::now();
        if (sent >= measure_end) {
            return false;
        }
        pending.push_back({id, sent});
        transfer(socket, wire.data(), wire.size(), true);
        ++result.sent;
        result.payload_bytes += options.payload_bytes;
        return true;
    };

    for (std::size_t index = 0; index < options.window; ++index) {
        if (!send_one()) {
            break;
        }
    }

    while (!pending.empty()) {
        std::array<unsigned char, 4> header{};
        transfer(socket, reinterpret_cast<char*>(header.data()), header.size(), false);
        std::uint32_t body_bytes = 0;
        for (const auto byte : header) {
            body_bytes = (body_bytes << 8) | byte;
        }
        if (body_bytes == 0 || body_bytes > 252) {
            throw std::runtime_error("invalid ACK frame length");
        }
        std::array<char, 252> body{};
        transfer(socket, body.data(), body_bytes, false);
        realtime::Ack ack;
        if (!ack.ParseFromArray(body.data(), static_cast<int>(body_bytes)) ||
            ack.request_id() != pending.front().request_id || ack.result() != realtime::OK ||
            !ack.error().empty()) {
            throw std::runtime_error("ACK parse, request_id or business result mismatch");
        }
        const auto received = Clock::now();
        const auto completed = pending.front();
        pending.pop_front();
        ++result.acknowledged;
        if (received >= measure_start && received < measure_end) {
            ++result.acknowledged_in_interval;
        }
        // 保存全部 ACK（含预热和排空），让离线分析可重算吞吐和样本选择。
        if (result.samples.size() >= sample_capacity) {
            throw std::runtime_error("sample capacity reached; shorten the run");
        }
        result.samples.push_back({completed.request_id,
            relative_ns(completed.sent, start),
            relative_ns(received, start)});
        if (received < measure_end) {
            send_one();
        }
    }
}

std::int64_t percentile(const std::vector<std::int64_t>& sorted, unsigned percent) {
    if (sorted.empty()) {
        throw std::runtime_error("no measurement samples");
    }
    // nearest-rank，和离线分析使用完全相同的定义。
    const auto rank = (sorted.size() * percent + 99) / 100;
    return sorted.at(rank - 1);
}

nlohmann::json write_results(const Options& options,
    const std::vector<ConnectionResult>& results,
    Clock::time_point finished,
    Clock::time_point start) {
    std::ofstream output(options.samples_path);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output << "connection,request_id,sent_ns,ack_ns,rtt_ns\n";
    std::vector<std::int64_t> latency;
    std::uint64_t sent = 0;
    std::uint64_t acknowledged = 0;
    std::uint64_t acknowledged_in_interval = 0;
    std::uint64_t payload_bytes = 0;
    nlohmann::json errors = nlohmann::json::array();
    for (std::size_t connection = 0; connection < results.size(); ++connection) {
        const auto& result = results[connection];
        sent += result.sent;
        acknowledged += result.acknowledged;
        acknowledged_in_interval += result.acknowledged_in_interval;
        payload_bytes += result.payload_bytes;
        if (!result.error.empty()) {
            errors.push_back({{"connection", connection}, {"error", result.error}});
        }
        for (const auto& sample : result.samples) {
            const auto rtt = sample.ack_ns - sample.sent_ns;
            const auto warmup_ns = static_cast<std::int64_t>(options.warmup_seconds) * 1000000000;
            const auto end_ns =
                warmup_ns + static_cast<std::int64_t>(options.measure_seconds) * 1000000000;
            if (sample.sent_ns >= warmup_ns && sample.sent_ns < end_ns) {
                latency.push_back(rtt);
            }
            output << connection << ',' << sample.request_id << ',' << sample.sent_ns << ','
                   << sample.ack_ns << ',' << rtt << '\n';
        }
    }
    output.close();
    std::sort(latency.begin(), latency.end());
    return {{"mode", "closed_loop"},
        {"sample_scope", "all_acknowledged"},
        {"start_monotonic_ns",
            std::chrono::duration_cast<Nanoseconds>(start.time_since_epoch()).count()},
        {"connections", options.connections},
        {"window_per_connection", options.window},
        {"payload_bytes_per_request", options.payload_bytes},
        {"warmup_seconds", options.warmup_seconds},
        {"measure_seconds", options.measure_seconds},
        {"sent_including_warmup", sent},
        {"acknowledged_including_warmup", acknowledged},
        {"acknowledged_in_measure_interval", acknowledged_in_interval},
        {"throughput_ack_per_second",
            static_cast<double>(acknowledged_in_interval) / options.measure_seconds},
        {"measurement_samples", latency.size()},
        {"p50_ns", percentile(latency, 50)},
        {"p95_ns", percentile(latency, 95)},
        {"p99_ns", percentile(latency, 99)},
        {"max_ns", latency.back()},
        {"payload_bytes_sent_including_warmup", payload_bytes},
        {"drain_ns",
            relative_ns(finished, start) -
                static_cast<std::int64_t>(options.warmup_seconds + options.measure_seconds) *
                    1000000000},
        {"errors", errors}};
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse_options(argc, argv);
        boost::asio::io_context io;
        std::vector<std::unique_ptr<tcp::socket>> sockets;
        for (std::size_t connection = 0; connection < options.connections; ++connection) {
            auto socket = std::make_unique<tcp::socket>(io);
            socket->connect({boost::asio::ip::make_address("127.0.0.1"), options.port});
            socket->set_option(tcp::no_delay(true));
            socket->non_blocking(true);
            sockets.push_back(std::move(socket));
        }
        std::vector<ConnectionResult> results(options.connections);
        Clock::time_point start;
        // 所有线程就绪后统一计时；连接建立不包含在测量中。
        std::barrier ready(static_cast<std::ptrdiff_t>(options.connections), [&start]() noexcept {
            start = Clock::now();
        });
        std::vector<std::jthread> threads;
        try {
            for (std::size_t connection = 0; connection < options.connections; ++connection) {
                threads.emplace_back([&, connection] {
                    ready.arrive_and_wait();
                    try {
                        run_connection(
                            *sockets[connection], options, connection, start, results[connection]);
                    } catch (const std::exception& error) {
                        results[connection].error = error.what();
                    }
                });
            }
        } catch (...) {
            // 创建线程失败时让未创建的参与者退出 barrier，不能让已创建线程永久等待。
            for (auto missing = threads.size(); missing < options.connections; ++missing) {
                ready.arrive_and_drop();
            }
            throw;
        }
        threads.clear();
        const auto finished = Clock::now();
        const auto summary = write_results(options, results, finished, start);
        std::cout << summary.dump(2) << '\n';
        if (!summary.at("errors").empty() ||
            summary.at("sent_including_warmup") != summary.at("acknowledged_including_warmup")) {
            return 1;
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
