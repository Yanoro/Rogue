#pragma once
#include <string>
#include <vector>
#include <functional>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <unordered_set>
#include <mutex>

class AI {
public:
  using StreamCallback = std::function<void(const std::string&)>;

  virtual std::string generate(const std::string& contextId, const std::string& prompt) = 0;
  virtual bool generateStream(const std::string& contextId, const std::string& prompt, StreamCallback callback) = 0;
  virtual bool isBusy(const std::string& contextId) = 0;
  virtual std::string getLastMessage(const std::string& contextId) = 0;
  virtual std::string getContext(const std::string& contextId) = 0;
  virtual ~AI() = default;
};

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
    OllamaAI* aiInstance;
    std::string fullResponse;
  };

  static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp);
  static size_t WriteCallbackStream(void* contents, size_t size, size_t nmemb, void* userp);

public:
  OllamaAI(const std::string& model = "llama3", const std::string& url = "http://localhost:11434/api/generate");

  void setOption(const std::string& key, const nlohmann::json& value);

  std::string generate(const std::string& contextId, const std::string& prompt) override;
  bool generateStream(const std::string& contextId, const std::string& prompt, StreamCallback callback) override;
  bool isBusy(const std::string& contextId) override;
  std::string getLastMessage(const std::string& contextId) override;
  std::string getContext(const std::string& contextId) override;
};
