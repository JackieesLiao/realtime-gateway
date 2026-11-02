#include "gateway/server.hpp"
#include "gateway/log.hpp"
#include <thread>
#include <vector>
#include <iostream>
int main(int argc, char** argv) {
    spdlog::set_pattern("%v");
    if (argc != 2) { std::cerr << "usage: gateway_server CONFIG.json\n"; return 2; }
    try {
        const auto config = gateway::Config::load(argv[1]);
        boost::asio::io_context io;
        auto server = std::make_shared<gateway::Server>(io, config);
        server->start();
        // jthread joins on scope exit, including partial thread-creation failure.
        std::vector<std::jthread> threads;
        try {
            for (std::size_t i = 1; i < config.io_threads; ++i)
                threads.emplace_back([&io] { io.run(); });
            io.run();
        } catch (...) { io.stop(); throw; }
        threads.clear();
        server->report_statistics();
        gateway::log({{"event", "stopped"}});
    } catch (const std::exception& e) {
        gateway::log({{"event", "fatal"}, {"error", e.what()}});
        return 1;
    }
}
