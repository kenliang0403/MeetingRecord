#include "ControlServer.h"
#include <spdlog/spdlog.h>
#include <boost/asio/read_until.hpp>

ControlServer::ControlServer(const std::string& bindAddr, int port)
    : acceptor_(ioc_)
    , bindAddr_(bindAddr)
    , port_(port)
{
}

ControlServer::~ControlServer()
{
    stop();
}

void ControlServer::registerHandler(const std::string& cmd, CommandHandler handler)
{
    handlers_[cmd] = std::move(handler);
}

bool ControlServer::start()
{
    try {
        auto endpoint = tcp::endpoint(asio::ip::make_address(bindAddr_), port_);
        acceptor_.open(endpoint.protocol());
        acceptor_.set_option(asio::socket_base::reuse_address(true));
        acceptor_.bind(endpoint);
        acceptor_.listen();

        running_ = true;
        thread_  = std::thread([this]{ acceptLoop(); });

        spdlog::info("ControlServer: listening on {}:{}", bindAddr_, port_);
        return true;
    } catch (const std::exception& e) {
        spdlog::error("ControlServer: start failed: {}", e.what());
        return false;
    }
}

void ControlServer::stop()
{
    if (running_.exchange(false)) {
        ioc_.stop();
        if (thread_.joinable()) thread_.join();
        spdlog::info("ControlServer: stopped");
    }
}

void ControlServer::acceptLoop()
{
    while (running_) {
        try {
            auto sock = std::make_shared<tcp::socket>(ioc_);
            acceptor_.accept(*sock);
            std::thread([this, sock = std::move(sock)]() mutable {
                handleClient(std::move(sock));
            }).detach();
        } catch (...) {
            if (running_) spdlog::warn("ControlServer: accept error");
        }
    }
}

void ControlServer::handleClient(std::shared_ptr<tcp::socket> sock)
{
    boost::system::error_code ec;
    asio::streambuf buf;

    while (running_) {
        asio::read_until(*sock, buf, '\n', ec);
        if (ec) break;

        std::istream is(&buf);
        std::string  line;
        std::getline(is, line);
        if (line.empty()) continue;

        nlohmann::json resp;
        try {
            auto req = nlohmann::json::parse(line);
            std::string cmd = req.value("cmd", "");

            auto it = handlers_.find(cmd);
            if (it == handlers_.end()) {
                resp = {{"ok", false}, {"error", "unknown command: " + cmd}};
            } else {
                resp = it->second(req);
            }
        } catch (const std::exception& e) {
            resp = {{"ok", false}, {"error", e.what()}};
        }

        std::string out = resp.dump() + "\n";
        asio::write(*sock, asio::buffer(out), ec);
        if (ec) break;
    }
}
