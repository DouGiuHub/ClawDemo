#include "novel_agent.h"

namespace clawstory {

using json = nlohmann::json;

NovelAgent::NovelAgent(LlmClient& llm) : llm_(llm) {}

void NovelAgent::generate(const std::string& story_idea,
                           DeltaCallback on_delta,
                           DoneCallback on_done,
                           ErrorCallback on_error,
                           ProviderCallback on_provider,
                           std::shared_ptr<std::atomic<bool>> abort_flag) {
    // Build system prompt for novel writing
    json messages = json::array();

    messages.push_back({
        {"role", "system"},
        {"content",
         R"(你是一位才华横溢的小说作家，擅长创作引人入胜的中文小说。

写作要求：
1. 情节紧凑，有起承转合，节奏张弛有度
2. 人物形象鲜明，对话生动自然，符合角色性格
3. 环境描写细腻，营造沉浸式的画面感
4. 善用修辞手法，语言优美但不堆砌
5. 保持文风统一，叙事视角一致

请根据用户给出的故事概念，直接创作小说正文。不要输出标题、大纲或任何说明性文字，只输出小说内容。)"}
    });

    messages.push_back({
        {"role", "user"},
        {"content", "请根据以下概念创作一个小说章节：\n\n" + story_idea}
    });

    llm_.stream_chat(messages,
        std::move(on_delta),
        std::move(on_done),
        std::move(on_error),
        std::move(on_provider),
        std::move(abort_flag));
}

void NovelAgent::generate(const std::string& story_idea,
                           const std::string& provider_name,
                           DeltaCallback on_delta,
                           DoneCallback on_done,
                           ErrorCallback on_error,
                           ProviderCallback on_provider,
                           std::shared_ptr<std::atomic<bool>> abort_flag) {
    json messages = json::array();
    messages.push_back({
        {"role", "system"},
        {"content",
         R"(你是一位才华横溢的小说作家，擅长创作引人入胜的中文小说。

写作要求：
1. 情节紧凑，有起承转合，节奏张弛有度
2. 人物形象鲜明，对话生动自然，符合角色性格
3. 环境描写细腻，营造沉浸式的画面感
4. 善用修辞手法，语言优美但不堆砌
5. 保持文风统一，叙事视角一致

请根据用户给出的故事概念，直接创作小说正文。不要输出标题、大纲或任何说明性文字，只输出小说内容。)"}
    });
    messages.push_back({
        {"role", "user"},
        {"content", "请根据以下概念创作一个小说章节：\n\n" + story_idea}
    });

    llm_.stream_chat(provider_name, messages,
        std::move(on_delta),
        std::move(on_done),
        std::move(on_error),
        std::move(on_provider),
        std::move(abort_flag));
}

} // namespace clawstory
