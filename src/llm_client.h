#pragma once
#include <string>
#include <functional>
#include <atomic>
#include <vector>
#include <mutex>
#include <nlohmann/json.hpp>

namespace clawstory {

/// Single provider configuration (Ollama, OpenAI, DeepSeek, etc.)
struct ProviderConfig {
    std::string name;       // e.g. "ollama", "deepseek"
    std::string api_key;
    std::string base_url;   // e.g. "http://localhost:11434/v1"
    std::string model;      // e.g. "qwen3:8b", "deepseek-chat"
    int max_tokens  = 4096;
    double temperature = 0.8;
    bool is_local   = false;   // true for Ollama (skip auth, shorter timeout)
};

/// LLM client with multi-provider failover (参考 clawcpp FailoverResolver).
/// Providers are tried in order; on connection/transient error, falls back to next.
class LlmClient {
public:
    /// Single-provider constructor (convenience)
    explicit LlmClient(ProviderConfig cfg);

    /// Multi-provider constructor (first = primary, rest = fallback chain)
    explicit LlmClient(std::vector<ProviderConfig> providers);

    using DeltaCallback = std::function<void(const std::string& delta)>;
    using DoneCallback  = std::function<void()>;
    using ErrorCallback = std::function<void(const std::string& err)>;
    using ProviderCallback = std::function<void(const std::string& provider, const std::string& model)>;

    /// Streaming chat completion with automatic failover.
    /// Tries providers in order until one succeeds or all fail.
    /// Calls on_provider when a provider is selected (including after failover).
    void stream_chat(const nlohmann::json& messages,
                     DeltaCallback on_delta,
                     DoneCallback on_done,
                     ErrorCallback on_error,
                     ProviderCallback on_provider,
                     std::shared_ptr<std::atomic<bool>> abort_flag);

    /// Streaming chat completion with a specific provider (no failover).
    void stream_chat(const std::string& provider_name,
                     const nlohmann::json& messages,
                     DeltaCallback on_delta,
                     DoneCallback on_done,
                     ErrorCallback on_error,
                     ProviderCallback on_provider,
                     std::shared_ptr<std::atomic<bool>> abort_flag);

    /// Check if a provider is reachable (HEAD request, short timeout).
    /// Returns provider name if available, "" if not.
    std::string check_health(size_t provider_idx) const;

    /// Probe all providers, return list of available names.
    std::vector<std::string> probe_all();

    /// Get current active provider name (after failover resolution).
    const std::string& active_provider() const;

    /// Get all provider configs (read-only).
    const std::vector<ProviderConfig>& providers() const;

private:
    /// Try streaming with a single provider. Returns true on success.
    bool try_stream(const ProviderConfig& cfg,
                    const nlohmann::json& messages,
                    DeltaCallback& on_delta,
                    DoneCallback& on_done,
                    ErrorCallback& on_error,
                    std::shared_ptr<std::atomic<bool>> abort_flag);

    std::vector<ProviderConfig> providers_;
    mutable std::mutex active_mutex_;
    std::string active_provider_;
};

} // namespace clawstory
