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

  // Second, independent mutex rather than reusing messagesMutex: the timing is
  // written from the curl worker (inside WriteCallbackStream) at the moment the
  // final chunk arrives, which is the same instant the end-of-stream sentinel is
  // dispatched to the caller. Keeping the two locks apart means a caller that
  // reacts to the sentinel by asking for the stats cannot interleave badly with
  // the worker still holding a lock.
  std::unordered_map<std::string, AIRequestStats> lastStats;
  std::mutex statsMutex;

  // Passed through as the request's `keep_alive`. Empty means "do not send the
  // field", which leaves Ollama's own default (5 minutes, or OLLAMA_KEEP_ALIVE)
  // in charge.
  std::string keepAlive;

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

  // How long Ollama should keep the model resident after a request, e.g. "30m".
  // An empty string sends nothing, leaving Ollama's own default in charge
  // (5 minutes, or the OLLAMA_KEEP_ALIVE environment variable). Keeping a model
  // warm is the difference between measuring inference and measuring disk I/O.
  void setKeepAlive(const std::string &value) { keepAlive = value; }

  std::string generate(const std::string &contextId,
                       const std::vector<ChatMessage> &history,
                       std::stop_token stoken = {},
                       const ImageList &images = {}) override;
  bool generateStream(const std::string &contextId, const std::vector<ChatMessage> &history,
                      StreamCallback callback,
                      std::stop_token stoken = {},
                      const ImageList &images = {}) override;
  bool isBusy(const std::string &contextId) override;
  std::string getLastMessage(const std::string &contextId) override;
  std::string getContext(const std::string &contextId) override;

  // Drops the stored token context and the recorded messages for `contextId`.
  // Ollama's /api/generate is stateful: it returns a `context` array that the
  // next call replays as conversation memory, which is how the NPC chat keeps
  // continuity. Clearing it is what makes a caller's next request independent.
  void resetContext(const std::string &contextId) override;

  // Timing/token breakdown of the last completed request, straight from the
  // final streaming chunk. See AI.h.
  bool getLastStats(const std::string &contextId, AIRequestStats &out) override;
  
  std::string getAIName() const override { return "Ollama"; }
  std::string getModelName() const override { return modelName; }
  std::string getProviderName() const override { return "Local (Ollama)"; }
  
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
