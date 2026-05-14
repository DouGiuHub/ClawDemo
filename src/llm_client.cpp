#include "llm_client.h"
#include <curl/curl.h>
#include <sstream>
#include <iostream>

namespace clawstory {

using json = nlohmann::json;

// ── Constructors ────────────────────────────────────────────────────────────

LlmClient::LlmClient(ProviderConfig cfg)
    : providers_{std::move(cfg)} {
    active_provider_ = providers_.front().name;
}

LlmClient::LlmClient(std::vector<ProviderConfig> providers)
    : providers_{std::move(providers)} {
    if (providers_.empty()) {
        throw std::invalid_argument("At least one provider is required");
    }
    active_provider_ = providers_.front().name;
}

const std::string& LlmClient::active_provider() const {
    std::lock_guard<std::mutex> lock(active_mutex_);
    return active_provider_;
}

const std::vector<ProviderConfig>& LlmClient::providers() const {
    return providers_;
}

// ── Health check ────────────────────────────────────────────────────────────

static size_t discard_cb(char*, size_t size, size_t nmemb, void*) {
    return size * nmemb;
}

std::string LlmClient::check_health(size_t idx) const {
    if (idx >= providers_.size()) return "";

    const auto& cfg = providers_[idx];
    std::string url = cfg.base_url;
    if (!url.empty() && url.back() == '/') url.pop_back();

    // For Ollama: check native API; for others: check /models endpoint
    if (cfg.is_local) {
        // Strip /v1 suffix to reach Ollama native API root
        auto v1_pos = url.rfind("/v1");
        if (v1_pos != std::string::npos) url = url.substr(0, v1_pos);
        url += "/api/tags";  // Ollama model list endpoint
    } else {
        url += "/models";
    }

    CURL* curl = curl_easy_init();
    if (!curl) return "";

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_cb);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);

    if (cfg.is_local) {
        // No auth needed for local Ollama
    } else {
        std::string auth = "Authorization: Bearer " + cfg.api_key;
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, auth.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        CURLcode res = curl_easy_perform(curl);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return (res == CURLE_OK) ? cfg.name : "";
    }

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return (res == CURLE_OK) ? cfg.name : "";
}

std::vector<std::string> LlmClient::probe_all() {
    std::vector<std::string> available;
    for (size_t i = 0; i < providers_.size(); ++i) {
        std::string name = check_health(i);
        if (!name.empty()) {
            available.push_back(name);
            std::cout << "  [OK]   " << providers_[i].name
                      << " (" << providers_[i].model
                      << " @ " << providers_[i].base_url << ")" << std::endl;
        } else {
            std::cout << "  [--]   " << providers_[i].name
                      << " (" << providers_[i].base_url << " unreachable)" << std::endl;
        }
    }
    return available;
}

// ── SSE stream context ──────────────────────────────────────────────────────

struct StreamCtx {
    std::string buffer;
    std::function<void(const std::string&)> on_delta;
    std::function<void()> on_done;
    std::function<void(const std::string&)> on_error;
    std::shared_ptr<std::atomic<bool>> abort_flag;
    bool finished = false;
    bool transient_error = false;  // connection-level error, can retry
};

// ── libcurl write callback ──────────────────────────────────────────────────

static size_t sse_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<StreamCtx*>(userdata);
    if (ctx->abort_flag && ctx->abort_flag->load()) return 0;

    ctx->buffer.append(ptr, size * nmemb);

    while (true) {
        size_t pos = ctx->buffer.find("\n\n");
        if (pos == std::string::npos) break;

        std::string event_block = ctx->buffer.substr(0, pos);
        ctx->buffer.erase(0, pos + 2);

        std::istringstream lines(event_block);
        std::string line;
        while (std::getline(lines, line)) {
            if (!line.starts_with("data: ")) continue;
            std::string payload = line.substr(6);
            if (!payload.empty() && payload.back() == '\r') payload.pop_back();

            if (payload == "[DONE]") {
                ctx->finished = true;
                if (ctx->on_done) ctx->on_done();
                return size * nmemb;
            }

            try {
                auto j = json::parse(payload);
                // Check for error response from API
                if (j.contains("error")) {
                    auto& err = j["error"];
                    std::string msg = err.value("message", "Unknown API error");
                    int code = err.value("code", 0);
                    // Auth/billing errors are NOT transient
                    if (code == 401 || code == 402 || code == 403) {
                        ctx->on_error("Provider error: " + msg);
                        ctx->finished = true;
                        return 0;
                    }
                    // Rate limit / server errors are transient → trigger failover
                    ctx->transient_error = true;
                    ctx->on_error("Transient error: " + msg);
                    ctx->finished = true;
                    return 0;
                }
                if (j.contains("choices") && j["choices"].is_array() && !j["choices"].empty()) {
                    auto& choice = j["choices"][0];
                    if (choice.contains("delta") && choice["delta"].is_object()) {
                        auto& delta = choice["delta"];
                        if (delta.contains("content") && delta["content"].is_string()) {
                            ctx->on_delta(delta["content"].get<std::string>());
                        }
                    }
                }
            } catch (const json::parse_error&) {}
        }
    }

    return size * nmemb;
}

// ── Single provider stream attempt ──────────────────────────────────────────

bool LlmClient::try_stream(const ProviderConfig& cfg,
                            const json& messages,
                            DeltaCallback& on_delta,
                            DoneCallback& on_done,
                            ErrorCallback& on_error,
                            std::shared_ptr<std::atomic<bool>> abort_flag) {
    StreamCtx ctx;
    ctx.on_delta   = on_delta;
    ctx.on_done    = on_done;
    ctx.on_error   = on_error;  // don't move — caller may reuse
    ctx.abort_flag = abort_flag;

    // Build URL
    std::string url = cfg.base_url;
    if (!url.empty() && url.back() == '/') url.pop_back();
    url += "/chat/completions";

    // Build body
    json body;
    body["model"]      = cfg.model;
    body["messages"]   = messages;
    body["max_tokens"] = cfg.max_tokens;
    body["temperature"]= cfg.temperature;
    body["stream"]     = true;
    std::string body_str = body.dump();

    CURL* curl = curl_easy_init();
    if (!curl) {
        // curl init failure is transient (unlikely)
        return false;
    }

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    // Ollama doesn't require Bearer token, but some setups accept any value
    if (!cfg.is_local || cfg.api_key != "ollama") {
        std::string auth = "Authorization: Bearer " + cfg.api_key;
        headers = curl_slist_append(headers, auth.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sse_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);

    // Local providers get shorter connect timeout for faster failover
    long connect_timeout = cfg.is_local ? 3L : 10L;
    long total_timeout   = cfg.is_local ? 60L : 300L;
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, connect_timeout);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, total_timeout);

    CURLcode res = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    // HTTP error (non-2xx) → parse the error body and fail over
    if (res == CURLE_OK && http_code >= 400) {
        std::cerr << "[LlmClient] " << cfg.name << " HTTP error " << http_code << std::endl;
        try {
            auto j = json::parse(ctx.buffer);
            if (j.contains("error")) {
                std::string msg = j["error"].value("message", ctx.buffer);
                ctx.on_error("Provider error (" + std::to_string(http_code) + "): " + msg);
            } else {
                ctx.on_error("Provider error (" + std::to_string(http_code) + "): " + ctx.buffer);
            }
        } catch (...) {
            ctx.on_error("Provider error (" + std::to_string(http_code) + "): " + ctx.buffer);
        }
        return false;
    }

    if (res == CURLE_OK && ctx.finished && !ctx.transient_error) {
        return true;   // success
    }

    // Connection-level curl errors → transient, can failover
    if (res != CURLE_OK) {
        return false;
    }

    // API returned a transient error → can failover
    if (ctx.transient_error) {
        return false;
    }

    // Non-transient API error (auth/billing) → don't retry this provider
    return true;  // but ctx.on_error was already called
}

// ── stream_chat with failover ───────────────────────────────────────────────

void LlmClient::stream_chat(const json& messages,
                             DeltaCallback on_delta,
                             DoneCallback on_done,
                             ErrorCallback on_error,
                             ProviderCallback on_provider,
                             std::shared_ptr<std::atomic<bool>> abort_flag) {
    if (abort_flag && abort_flag->load()) return;

    std::string last_error;

    for (size_t i = 0; i < providers_.size(); ++i) {
        if (abort_flag && abort_flag->load()) return;

        const auto& cfg = providers_[i];
        std::cout << "[LlmClient] trying " << cfg.name
                  << " (" << cfg.model << ")..." << std::endl;

        // For local providers, quick health check before full request
        if (cfg.is_local && check_health(i).empty()) {
            last_error = cfg.name + " is unreachable";
            std::cout << "[LlmClient] " << cfg.name << " unreachable, skipping" << std::endl;
            continue;
        }

        // Notify which provider is being used
        if (on_provider) on_provider(cfg.name, cfg.model);

        bool ok = try_stream(cfg, messages, on_delta, on_done, on_error, abort_flag);
        if (ok) {
            {
                std::lock_guard<std::mutex> lock(active_mutex_);
                active_provider_ = cfg.name;
            }
            std::cout << "[LlmClient] using " << cfg.name << std::endl;
            return;
        }

        last_error = cfg.name + " request failed";
        if (i + 1 < providers_.size()) {
            std::cout << "[LlmClient] " << cfg.name << " failed, failing over to "
                      << providers_[i + 1].name << std::endl;
        }
    }

    // All providers exhausted
    if (on_error) {
        on_error("All providers failed. Last error: " + last_error);
    }
}

void LlmClient::stream_chat(const std::string& provider_name,
                             const json& messages,
                             DeltaCallback on_delta,
                             DoneCallback on_done,
                             ErrorCallback on_error,
                             ProviderCallback on_provider,
                             std::shared_ptr<std::atomic<bool>> abort_flag) {
    if (abort_flag && abort_flag->load()) return;

    for (size_t i = 0; i < providers_.size(); ++i) {
        if (providers_[i].name != provider_name) continue;

        if (abort_flag && abort_flag->load()) return;

        std::cout << "[LlmClient] using specified provider " << provider_name
                  << " (" << providers_[i].model << ")" << std::endl;

        if (on_provider) on_provider(providers_[i].name, providers_[i].model);

        bool ok = try_stream(providers_[i], messages, on_delta, on_done, on_error, abort_flag);
        if (ok) {
            std::lock_guard<std::mutex> lock(active_mutex_);
            active_provider_ = providers_[i].name;
            return;
        }

        if (on_error) {
            on_error("Provider " + provider_name + " failed");
        }
        return;
    }

    if (on_error) {
        on_error("Provider " + provider_name + " not found");
    }
}

} // namespace clawstory
