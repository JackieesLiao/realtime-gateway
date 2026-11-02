#pragma once
#include <utility>
#include <boost/asio.hpp>
#include "gateway/config.hpp"
#include "gateway/business.hpp"
#include <memory>
#include <unordered_map>
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
    boost::asio::io_context& io_;
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
}
