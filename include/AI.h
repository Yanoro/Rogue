#pragma once
#include <string>
#include <vector>
#include <functional>
#include <nlohmann/json.hpp>

class AI {
public:
  using StreamCallback = std::function<void(const std::string&)>;

  virtual std::string generate(const std::string& prompt) = 0;
  virtual void generateStream(const std::string& prompt, StreamCallback callback) = 0;
  virtual ~AI() = default;
};

class OllamaAI : public AI {
private:
  std::string modelName;
  std::string endpoint;
  nlohmann::json options;
  std::vector<int> context;

  struct StreamContext {
    StreamCallback callback;
    std::string buffer;
  };

  static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp);
  static size_t WriteCallbackStream(void* contents, size_t size, size_t nmemb, void* userp);

public:
  OllamaAI(const std::string& model = "llama3", const std::string& url = "http://localhost:11434/api/generate");

  void setOption(const std::string& key, const nlohmann::json& value);

  std::string generate(const std::string& prompt) override;
  void generateStream(const std::string& prompt, StreamCallback callback) override;
};
