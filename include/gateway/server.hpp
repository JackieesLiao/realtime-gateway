#pragma once

#include "gateway/business.hpp"
#include "gateway/config.hpp"

#include <memory>
#include <unordered_map>
#include <utility>

#include <boost/asio.hpp>

namespace gateway {

class Session;

class Server : public std::enable_shared_from_this<Server> {
public:
    Server(boost::asio::io_context& io, Config config);

    void start();
    void stop();
    void report_statistics() const;

private:
    void accept();
    void handle_accept(boost::system::error_code error,
        const std::shared_ptr<boost::asio::ip::tcp::socket>& socket);

    boost::asio::io_context& io_;

    // The management strand owns the listener, signals and connection registry.
    boost::asio::strand<boost::asio::io_context::executor_type> strand_;
    boost::asio::ip::tcp::acceptor acceptor_;
    boost::asio::signal_set signals_;

    Config config_;
    Statistics stats_;
    Dispatcher dispatcher_;
    std::unordered_map<std::uint64_t, std::shared_ptr<Session>> sessions_;
    std::uint64_t next_id_ = 1;
    bool stopped_ = false;
};

} // namespace gateway
