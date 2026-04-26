#pragma once

#include <boost/asio.hpp>
#include <nlohmann/json.hpp>
#include <thread>
#include <atomic>
#include <string>
#include <functional>

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

/**
 * ControlServer — TCP JSON command server on port 9001.
 *
 * Protocol (one JSON object per line):
 *   Request:  {"cmd": "status"}
 *             {"cmd": "stop_record", "token": "<call-token>"}
 *
 *   Response: {"ok": true, "data": {...}}
 *             {"ok": false, "error": "..."}
 *
 * Used by the Java/Spring Boot backend to query and control the C++ core.
 */
class ControlServer {
public:
    using CommandHandler = std::function<
        nlohmann::json(const nlohmann::json& req)>;

    ControlServer(const std::string& bindAddr, int port);
    ~ControlServer();

    void registerHandler(const std::string& cmd, CommandHandler handler);
    bool start();
    void stop();

private:
    void acceptLoop();
    void handleClient(std::shared_ptr<tcp::socket> sock);

    asio::io_context         ioc_;
    tcp::acceptor            acceptor_;
    std::thread              thread_;
    std::atomic<bool>        running_{false};

    std::unordered_map<std::string, CommandHandler> handlers_;
    std::string bindAddr_;
    int         port_;
};
