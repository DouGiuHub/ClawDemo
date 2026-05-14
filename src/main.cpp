#include "llm_client.h"
#include "ws_server.h"
#include "novel_agent.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <csignal>
#include <curl/curl.h>

using json = nlohmann::json;

std::atomic<bool> g_shutdown{false};

static std::string get_executable_dir() {
    char path[4096]{};
    ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (len == -1) return "";
    path[len] = '\0';
    std::string s(path);
    auto pos = s.find_last_of('/');
    return (pos == std::string::npos) ? "" : s.substr(0, pos);
}

static void signal_handler(int) {
    g_shutdown = true;
}

static json load_config(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        std::cerr << "Config not found: " << path << std::endl;
        std::cerr << "Create config.json. Example:\n" << std::endl;
        std::cerr << R"({
  "providers": [
    {
      "name": "ollama",
      "api_key": "ollama",
      "base_url": "http://localhost:11434/v1",
      "model": "qwen3:8b",
      "max_tokens": 4096,
      "temperature": 0.8,
      "is_local": true
    },
    {
      "name": "deepseek",
      "api_key": "sk-xxx",
      "base_url": "https://api.deepseek.com/v1",
      "model": "deepseek-chat",
      "max_tokens": 4096,
      "temperature": 0.8
    }
  ],
  "server": { "ws_port": 8765, "http_port": 8080, "host": "127.0.0.1" }
})" << std::endl;
        exit(1);
    }
    try {
        return json::parse(f);
    } catch (const json::parse_error& e) {
        std::cerr << "Invalid config.json: " << e.what() << std::endl;
        exit(1);
    }
}

/// Parse providers array from config. Supports both new "providers" array
/// and legacy single "llm" object for backward compatibility.
static std::vector<clawstory::ProviderConfig> parse_providers(const json& config) {
    std::vector<clawstory::ProviderConfig> result;

    if (config.contains("providers") && config["providers"].is_array()) {
        for (const auto& p : config["providers"]) {
            result.push_back(clawstory::ProviderConfig{
                .name        = p.value("name", "unknown"),
                .api_key     = p.value("api_key", ""),
                .base_url    = p.value("base_url", "https://api.openai.com/v1"),
                .model       = p.value("model", "gpt-4o"),
                .max_tokens  = p.value("max_tokens", 4096),
                .temperature = p.value("temperature", 0.8),
                .is_local    = p.value("is_local", false),
            });
        }
    } else if (config.contains("llm")) {
        // Legacy single-provider format
        const auto& llm = config["llm"];
        result.push_back(clawstory::ProviderConfig{
            .name        = "default",
            .api_key     = llm.value("api_key", ""),
            .base_url    = llm.value("base_url", "https://api.openai.com/v1"),
            .model       = llm.value("model", "gpt-4o"),
            .max_tokens  = llm.value("max_tokens", 4096),
            .temperature = llm.value("temperature", 0.8),
            .is_local    = false,
        });
    }

    return result;
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    curl_global_init(CURL_GLOBAL_DEFAULT);

    std::string config_path;
    if (argc > 1) {
        config_path = argv[1];
    } else {
        std::string exe_dir = get_executable_dir();
        if (!exe_dir.empty()) {
            std::string exe_cfg = exe_dir + "/config.json";
            std::ifstream test(exe_cfg);
            if (test.is_open()) {
                config_path = exe_cfg;
            }
        }
        if (config_path.empty()) {
            config_path = "config.json";
        }
    }

    json config = load_config(config_path);

    // Parse providers (Ollama first → remote fallback)
    auto provider_configs = parse_providers(config);
    if (provider_configs.empty()) {
        std::cerr << "No providers configured. Add 'providers' array to config.json." << std::endl;
        return 1;
    }

    // Parse server config
    auto srv_cfg = clawstory::ServerConfig{
        .host     = config["server"].value("host", "127.0.0.1"),
        .ws_port  = config["server"].value("ws_port", 8765),
        .http_port= config["server"].value("http_port", 8080),
    };

    // Create LLM client with failover chain
    clawstory::LlmClient llm(std::move(provider_configs));

    // Create agent
    clawstory::NovelAgent agent(llm);

    // Start HTTP server for web UI
    std::string html_path = CLAWSTORY_WEB_DIR "/index.html";
    auto http_handle = clawstory::start_http_server(html_path, srv_cfg.http_port);

    // Create WebSocket server
    clawstory::WsServer ws_srv(std::move(srv_cfg));

    // Build providers list for frontend
    std::vector<std::pair<std::string, std::string>> provider_list;
    for (const auto& p : llm.providers()) {
        provider_list.emplace_back(p.name, p.model);
    }
    ws_srv.set_providers(provider_list);

    ws_srv.set_on_generate([&agent, &llm](const std::string& story_idea,
                                           const std::string& provider_name,
                                           auto on_delta, auto on_done, auto on_error,
                                           auto on_provider,
                                           auto abort_flag) {
        if (!provider_name.empty()) {
            agent.generate(story_idea, provider_name,
                           std::move(on_delta), std::move(on_done),
                           std::move(on_error), std::move(on_provider),
                           std::move(abort_flag));
        } else {
            agent.generate(story_idea, std::move(on_delta), std::move(on_done),
                           std::move(on_error), std::move(on_provider),
                           std::move(abort_flag));
        }
    });

    ws_srv.set_on_check_provider([&llm](const std::string& provider_name,
                                         auto on_result) {
        // Find provider index by name
        const auto& providers = llm.providers();
        for (size_t i = 0; i < providers.size(); ++i) {
            if (providers[i].name == provider_name) {
                std::string result = llm.check_health(i);
                if (!result.empty()) {
                    on_result(true, provider_name + " (" + providers[i].model + ") OK");
                } else {
                    on_result(false, provider_name + " (" + providers[i].model + ") unreachable");
                }
                return;
            }
        }
        on_result(false, "Provider '" + provider_name + "' not found");
    });

    // Open browser
    std::string url = "http://127.0.0.1:" + std::to_string(srv_cfg.http_port);
    std::cout << "\nOpen in browser: " << url << std::endl;
    std::string cmd = "xdg-open " + url + " 2>/dev/null &";
    std::system(cmd.c_str());

    // Block until stopped
    ws_srv.start();
    ws_srv.stop();
    http_handle.join();
    curl_global_cleanup();
    return 0;
}
