#pragma once
#include <string>
#include <functional>
#include <atomic>
#include <memory>
#include <nlohmann/json.hpp>
#include "llm_client.h"

namespace clawstory {

class NovelAgent {
public:
    explicit NovelAgent(LlmClient& llm);

    using DeltaCallback = std::function<void(const std::string&)>;
    using DoneCallback  = std::function<void()>;
    using ErrorCallback = std::function<void(const std::string&)>;
    using ProviderCallback = std::function<void(const std::string& provider, const std::string& model)>;

    /// Generate novel text from a concept, streaming results via callbacks.
    void generate(const std::string& story_idea,
                  DeltaCallback on_delta,
                  DoneCallback on_done,
                  ErrorCallback on_error,
                  ProviderCallback on_provider,
                  std::shared_ptr<std::atomic<bool>> abort_flag);

    /// Generate with a specific provider (no failover).
    void generate(const std::string& story_idea,
                  const std::string& provider_name,
                  DeltaCallback on_delta,
                  DoneCallback on_done,
                  ErrorCallback on_error,
                  ProviderCallback on_provider,
                  std::shared_ptr<std::atomic<bool>> abort_flag);

private:
    LlmClient& llm_;
};

} // namespace clawstory
