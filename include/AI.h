#pragma once
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <stop_token>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class AI {
public:
  using StreamCallback = std::function<void(const std::string &)>;
  std::function<void(const std::string&)> errorLogger;
  void setErrorLogger(std::function<void(const std::string&)> logger) {
    errorLogger = logger;
  }

  virtual std::string generate(const std::string &contextId,
                               const std::string &prompt,
                               std::stop_token stoken = {}) = 0;
  virtual bool generateStream(const std::string &contextId,
                              const std::string &prompt,
                              StreamCallback callback,
                              std::stop_token = {}) = 0;
  virtual bool isBusy(const std::string &contextId) = 0;
  virtual std::string getLastMessage(const std::string &contextId) = 0;
  virtual std::string getContext(const std::string &contextId) = 0;
  virtual ~AI() = default;
};
