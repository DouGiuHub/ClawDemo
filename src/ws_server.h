#pragma once
#include <string>
#include <functional>
#include <memory>
#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <nlohmann/json.hpp>
#include <ixwebsocket/IXWebSocketServer.h>

namespace clawstory {

struct ServerConfig {
    std::string host = "127.0.0.1";
    int ws_port   = 8765;
    int http_port = 8080;
};

class WsServer {
public:
    explicit WsServer(ServerConfig cfg);
    ~WsServer();

    using GenerateCallback = std::function<void(
        const std::string& story_idea,
        const std::string& provider_name,
        std::function<void(const std::string& delta)> on_delta,
        std::function<void()> on_done,
        std::function<void(const std::string& err)> on_error,
        std::function<void(const std::string& provider, const std::string& model)> on_provider,
        std::shared_ptr<std::atomic<bool>> abort_flag
    )>;

    using CheckProviderCallback = std::function<void(
        const std::string& provider_name,
        std::function<void(bool ok, const std::string& msg)> on_result
    )>;

    void set_on_generate(GenerateCallback cb);
    void set_on_check_provider(CheckProviderCallback cb);
    void set_providers(const std::vector<std::pair<std::string, std::string>>& providers);
    void start();  // blocking
    void stop();

private:
    struct ConnInfo {
        std::shared_ptr<std::atomic<bool>> abort_flag = std::make_shared<std::atomic<bool>>(false);
        std::thread generate_thread;
    };

    void on_message(const std::string& conn_id,
                    ix::WebSocket& ws,
                    const std::string& msg_str);
    void send_json(ix::WebSocket& ws, const nlohmann::json& msg);

    ServerConfig cfg_;
    GenerateCallback on_generate_;
    CheckProviderCallback on_check_provider_;
    std::vector<std::pair<std::string, std::string>> providers_;
    std::unique_ptr<ix::WebSocketServer> server_;
    std::mutex conn_mutex_;
    std::unordered_map<std::string, ConnInfo> connections_;
    std::atomic<bool> running_{false};
};

struct HttpServerHandle {
    std::thread thread;
    void join() { if (thread.joinable()) thread.join(); }
};

/// Simple HTTP server to serve the web UI (single HTML file).
HttpServerHandle start_http_server(const std::string& html_path, int port);

} // namespace clawstory
