#pragma once
#include "AI.h"

class OllamaAI : public AI {
private:
  std::string modelName;
  std::string endpoint;
  nlohmann::json options;
  std::unordered_map<std::string, std::vector<int>> contexts;
  std::unordered_set<std::string> busyContexts;
  std::mutex busyMutex;
  std::unordered_map<std::string, std::string> lastMessages;
  std::unordered_map<std::string, std::string> fullContexts;
  std::mutex messagesMutex;

  struct StreamContext {
    StreamCallback callback;
    std::string buffer;
    std::string contextId;
    OllamaAI *aiInstance;
    std::string fullResponse;
  };

  static size_t WriteCallback(void *contents, size_t size, size_t nmemb,
                              void *userp);
  static size_t WriteCallbackStream(void *contents, size_t size, size_t nmemb,
                                    void *userp);

public:
  OllamaAI(const std::string &model = "llama3",
           const std::string &url = "http://localhost:11434/api/generate");

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
  
  std::string getAIName() const override { return "Ollama"; }
  std::string getModelName() const override { return modelName; }
  
  std::string getAdditionalInfo() const override {
    std::string info;
    if (!options.empty()) {
      info += "Options:\n";
      for (auto& el : options.items()) {
        info += "  " + el.key() + ": " + el.value().dump() + "\n";
      }
    } else {
      info += "Options: Default\n";
    }
    return info;
  }
};
