#include "gateway/server.hpp"

#include "gateway/log.hpp"

#include <iostream>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
    spdlog::set_pattern("%v");
    if (argc != 2) {
        std::cerr << "usage: gateway_server CONFIG.json\n";
        return 2;
    }

    try {
        const auto config = gateway::Config::load(argv[1]);
        if (config.log_level == "debug") {
            spdlog::set_level(spdlog::level::debug);
        } else {
            spdlog::set_level(spdlog::level::info);
        }
        boost::asio::io_context io;
        auto server = std::make_shared<gateway::Server>(io, config);
        server->start();

        // The main thread is also an I/O thread. jthread joins workers on scope exit,
        // including when creating a later thread fails.
        std::vector<std::jthread> io_threads;
        try {
            for (std::size_t thread_index = 1; thread_index < config.io_threads; ++thread_index) {
                io_threads.emplace_back([&io] {
                    io.run();
                });
            }

            io.run();
        } catch (...) {
            io.stop();
            // 异常退出也要先回收业务线程，不能让它们向已销毁的 io_context 回送。
            server->join_workers();
            throw;
        }

        io_threads.clear();
        server->join_workers();
        server->report_statistics();
        gateway::log({{"event", "stopped"}});
    } catch (const std::exception& error) {
        gateway::log({{"event", "fatal"}, {"error", error.what()}});
        return 1;
    }
}
