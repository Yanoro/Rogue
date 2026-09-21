#pragma once
#include "AI.h"
#include <atomic>

class OpenRouterAI : public AI {
private:
  std::string apiKey;
  std::string modelName;
  std::unordered_map<std::string, std::vector<nlohmann::json>> contexts;
  std::unordered_set<std::string> busyContexts;
  std::mutex busyMutex;
  std::unordered_map<std::string, std::string> lastMessages;
  std::unordered_map<std::string, std::string> fullContexts;
  std::mutex messagesMutex;
  nlohmann::json options;
  std::string currentProvider = "Unknown";
  std::string currentQuantization = "Unknown";
  std::string rawMetadata = "";

  struct StreamContext {
    StreamCallback callback;
    std::string buffer;
    std::string contextId;
    OpenRouterAI *aiInstance;
    std::string fullResponse;
    bool isThinking = false;
  };

  std::atomic<int> totalRequests{0};

  static size_t WriteCallback(void *contents, size_t size, size_t nmemb,
                              void *userp);
  static size_t WriteCallbackStream(void *contents, size_t size, size_t nmemb,
                                    void *userp);

public:
  OpenRouterAI(const std::string &apiKey, const std::string &model = "openai/gpt-3.5-turbo");

  void setOption(const std::string &key, const nlohmann::json &value);

  std::string generate(const std::string &contextId,
                       const std::vector<ChatMessage> &history,
                       std::stop_token stoken = {}) override;
  bool generateStream(const std::string &contextId, const std::vector<ChatMessage> &history,
                      StreamCallback callback,
                      std::stop_token stoken = {}) override;
  bool isBusy(const std::string &contextId) override;
  std::string getLastMessage(const std::string &contextId) override;
  std::string getContext(const std::string &contextId) override;

  std::string getAIName() const override { return "OpenRouter"; }
  std::string getModelName() const override { return modelName; }
  std::string getProviderName() const override { return currentProvider; }
  std::string getQuantization() const override { return currentQuantization; }
  
  std::string getAdditionalInfo() const override {
    std::string info;
    info += "Total Requests: " + std::to_string(totalRequests.load()) + "\n\n";
    if (!options.empty()) {
      info += "Options:\n";
      for (auto& el : options.items()) {
        if (el.key() == "provider" && el.value().is_object()) {
            info += "  Provider Settings:\n";
            for (auto& provider_el : el.value().items()) {
                info += "    - " + provider_el.key() + ": " + provider_el.value().dump() + "\n";
            }
        } else {
            info += "  " + el.key() + ": " + el.value().dump() + "\n";
        }
      }
    } else {
      info += "Options: Default\n";
    }
    if (!rawMetadata.empty()) {
      info += "\nMetadata:\n" + rawMetadata + "\n";
    }
    return info;
  }
};
