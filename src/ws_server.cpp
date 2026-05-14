#include "ws_server.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <thread>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cerrno>

extern std::atomic<bool> g_shutdown;

namespace clawstory {

using json = nlohmann::json;

// ── WsServer ────────────────────────────────────────────────────────────────

WsServer::WsServer(ServerConfig cfg) : cfg_(std::move(cfg)) {
    server_ = std::make_unique<ix::WebSocketServer>(
        cfg_.ws_port, cfg_.host);
}

WsServer::~WsServer() { stop(); }

void WsServer::set_on_generate(GenerateCallback cb) {
    on_generate_ = std::move(cb);
}

void WsServer::set_on_check_provider(CheckProviderCallback cb) {
    on_check_provider_ = std::move(cb);
}

void WsServer::set_providers(const std::vector<std::pair<std::string, std::string>>& providers) {
    providers_ = providers;
}

void WsServer::start() {
    if (running_.exchange(true)) return;

    server_->setOnConnectionCallback(
        [this](std::weak_ptr<ix::WebSocket> ws_weak,
               std::shared_ptr<ix::ConnectionState> conn_state) {
            auto ws = ws_weak.lock();
            if (!ws) return;

            std::string conn_id = conn_state->getId();
            ws->setPingInterval(30);

            {
                std::lock_guard<std::mutex> lock(conn_mutex_);
                connections_[conn_id] = ConnInfo{};
            }

            // Send available providers list on connect
            if (!providers_.empty()) {
                json providers_msg;
                providers_msg["type"] = "providers";
                providers_msg["providers"] = json::array();
                for (const auto& [name, model] : providers_) {
                    providers_msg["providers"].push_back({{"name", name}, {"model", model}});
                }
                ws->send(providers_msg.dump());
            }

            ws->setOnMessageCallback(
                [this, conn_id, ws_raw = ws.get()](const ix::WebSocketMessagePtr& msg) {
                    if (msg->type == ix::WebSocketMessageType::Message) {
                        on_message(conn_id, *ws_raw, msg->str);
                    } else if (msg->type == ix::WebSocketMessageType::Close) {
                        std::lock_guard<std::mutex> lock(conn_mutex_);
                        auto it = connections_.find(conn_id);
                        if (it != connections_.end()) {
                            if (it->second.abort_flag) it->second.abort_flag->store(true);
                            if (it->second.generate_thread.joinable()) it->second.generate_thread.detach();
                            connections_.erase(it);
                        }
                    }
                });
        });

    auto res = server_->listen();
    if (!res.first) {
        std::cerr << "WebSocket server failed: " << res.second << std::endl;
        running_ = false;
        return;
    }
    server_->start();

    std::cout << "WebSocket server on ws://" << cfg_.host << ":" << cfg_.ws_port << std::endl;

    while (running_.load() && !g_shutdown.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

void WsServer::stop() {
    if (!running_.exchange(false)) return;
    // Abort all active generations
    {
        std::lock_guard<std::mutex> lock(conn_mutex_);
        for (auto& [id, info] : connections_) {
            info.abort_flag->store(true);
            if (info.generate_thread.joinable()) info.generate_thread.detach();
        }
        connections_.clear();
    }
    server_->stop();
}

void WsServer::on_message(const std::string& conn_id,
                           ix::WebSocket& ws,
                           const std::string& msg_str) {
    json parsed;
    try {
        parsed = json::parse(msg_str);
    } catch (...) {
        json err;
        err["type"] = "error";
        err["message"] = "Invalid JSON";
        send_json(ws, err);
        return;
    }

    std::string type = parsed.value("type", "");

    if (type == "generate") {
        std::string story_idea = parsed.value("concept", "");
        std::string provider_name = parsed.value("provider", "");
        if (story_idea.empty()) {
            json err;
            err["type"] = "error";
            err["message"] = "Missing 'concept' field";
            send_json(ws, err);
            return;
        }

        if (!on_generate_) {
            json err;
            err["type"] = "error";
            err["message"] = "No generate handler registered";
            send_json(ws, err);
            return;
        }

        // Get or create connection info
        std::shared_ptr<std::atomic<bool>> abort_flag;
        {
            std::lock_guard<std::mutex> lock(conn_mutex_);
            auto it = connections_.find(conn_id);
            if (it == connections_.end()) return;
            it->second.abort_flag->store(false);
            abort_flag = it->second.abort_flag;

            // Join previous thread if any
            if (it->second.generate_thread.joinable()) {
                it->second.abort_flag->store(true);
                it->second.generate_thread.join();
                it->second.abort_flag->store(false);
                abort_flag = it->second.abort_flag;
            }
        }

        // Spawn generation thread
        std::thread gen_thread([this, story_idea, provider_name, abort_flag, &ws]() {
            on_generate_(
                story_idea,
                provider_name,
                [this, &ws, abort_flag](const std::string& delta) {
                    if (abort_flag->load()) return;
                    json msg;
                    msg["type"] = "text_delta";
                    msg["delta"] = delta;
                    send_json(ws, msg);
                },
                [this, &ws, abort_flag]() {
                    if (abort_flag->load()) return;
                    json msg;
                    msg["type"] = "message_end";
                    send_json(ws, msg);
                },
                [this, &ws, abort_flag](const std::string& err) {
                    if (abort_flag->load()) return;
                    json msg;
                    msg["type"] = "error";
                    msg["message"] = err;
                    send_json(ws, msg);
                },
                [this, &ws, abort_flag](const std::string& provider, const std::string& model) {
                    if (abort_flag->load()) return;
                    json msg;
                    msg["type"] = "provider";
                    msg["provider"] = provider;
                    msg["model"] = model;
                    send_json(ws, msg);
                },
                abort_flag
            );
        });

        {
            std::lock_guard<std::mutex> lock(conn_mutex_);
            auto it = connections_.find(conn_id);
            if (it != connections_.end()) {
                it->second.generate_thread = std::move(gen_thread);
            } else {
                gen_thread.detach();
            }
        }

    } else if (type == "abort") {
        std::lock_guard<std::mutex> lock(conn_mutex_);
        auto it = connections_.find(conn_id);
        if (it != connections_.end()) {
            it->second.abort_flag->store(true);
        }
        json ack;
        ack["type"] = "abort.ack";
        send_json(ws, ack);

    } else if (type == "get_providers") {
        json providers_msg;
        providers_msg["type"] = "providers";
        providers_msg["providers"] = json::array();
        for (const auto& [name, model] : providers_) {
            providers_msg["providers"].push_back({{"name", name}, {"model", model}});
        }
        send_json(ws, providers_msg);

    } else if (type == "check_provider") {
        std::string provider_name = parsed.value("provider", "");
        if (provider_name.empty() || !on_check_provider_) {
            json resp;
            resp["type"] = "check_provider_result";
            resp["provider"] = provider_name;
            resp["ok"] = false;
            resp["message"] = provider_name.empty() ? "No provider specified" : "No check handler";
            send_json(ws, resp);
        } else {
            std::thread check_thread([this, provider_name, &ws]() {
                on_check_provider_(provider_name, [this, &ws, provider_name](bool ok, const std::string& msg) {
                    json resp;
                    resp["type"] = "check_provider_result";
                    resp["provider"] = provider_name;
                    resp["ok"] = ok;
                    resp["message"] = msg;
                    send_json(ws, resp);
                });
            });
            check_thread.detach();
        }

    } else {
        json err;
        err["type"] = "error";
        err["message"] = "Unknown type: " + type;
        send_json(ws, err);
    }
}

void WsServer::send_json(ix::WebSocket& ws, const json& msg) {
    try {
        ws.send(msg.dump());
    } catch (...) {}
}

// ── Simple HTTP server for web UI ───────────────────────────────────────────

static std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

HttpServerHandle start_http_server(const std::string& html_path, int port) {
    std::string html = read_file(html_path);
    HttpServerHandle handle;
    if (html.empty()) {
        std::cerr << "Warning: web UI not found at " << html_path << std::endl;
        return handle;
    }

    handle.thread = std::thread([html, port]() {
        int server_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd < 0) return;

        int opt = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        setsockopt(server_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        struct sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);

        if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(server_fd);
            return;
        }
        if (listen(server_fd, 10) < 0) {
            close(server_fd);
            return;
        }

        std::cout << "HTTP server on http://127.0.0.1:" << port << std::endl;

        while (!g_shutdown.load()) {
            int client_fd = accept(server_fd, nullptr, nullptr);
            if (client_fd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
                break;
            }

            char buf[1024]{};
            read(client_fd, buf, sizeof(buf) - 1);

            std::string resp =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/html; charset=utf-8\r\n"
                "Connection: close\r\n"
                "Content-Length: " + std::to_string(html.size()) + "\r\n"
                "\r\n" + html;
            send(client_fd, resp.data(), resp.size(), 0);
            close(client_fd);
        }
        close(server_fd);
    });
    return handle;
}

} // namespace clawstory
