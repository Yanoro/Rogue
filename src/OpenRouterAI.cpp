#include "OpenRouterAI.h"
#include <curl/curl.h>
#include <iostream>
#include <thread>

size_t OpenRouterAI::WriteCallback(void *contents, size_t size, size_t nmemb,
                                   void *userp) {
  ((std::string *)userp)->append((char *)contents, size * nmemb);
  return size * nmemb;
}

size_t OpenRouterAI::WriteCallbackStream(void *contents, size_t size, size_t nmemb,
                                         void *userp) {
  size_t real_size = size * nmemb;
  StreamContext *ctx = (StreamContext *)userp;

  ctx->buffer.append((char *)contents, real_size);

  size_t pos = 0;
  while ((pos = ctx->buffer.find('\n')) != std::string::npos) {
    std::string line = ctx->buffer.substr(0, pos);
    ctx->buffer.erase(0, pos + 1);

    if (line.empty() || line == "\r")
      continue;

    if (line.rfind(":", 0) == 0) {
      continue;
    }

    if (line.rfind("data: ", 0) == 0) {
      line = line.substr(6);
      if (line == "[DONE]" || line == "[DONE]\r") continue;
    }
    
    try {
      auto j = nlohmann::json::parse(line);
      if (j.contains("error")) {
        std::string errStr = "OpenRouter Stream Error: " + j["error"].dump();
        if (ctx->aiInstance->errorLogger) ctx->aiInstance->errorLogger(errStr);
        else std::cerr << errStr << std::endl;
      } else if (j.contains("choices") && j["choices"].is_array() && !j["choices"].empty()) {
        auto &choice = j["choices"][0];
        if (choice.contains("delta")) {
          auto &delta = choice["delta"];
          if (delta.contains("reasoning") && delta["reasoning"].is_string()) {
            std::string token = delta["reasoning"].get<std::string>();
            if (!token.empty()) {
              if (!ctx->isThinking) {
                ctx->isThinking = true;
                std::string tag = "<think>\n";
                ctx->fullResponse += tag;
                ctx->callback(tag);
              }
              ctx->fullResponse += token;
              ctx->callback(token);
            }
          }
          if (delta.contains("content") && delta["content"].is_string()) {
            std::string token = delta["content"].get<std::string>();
            if (!token.empty()) {
              if (ctx->isThinking) {
                ctx->isThinking = false;
                std::string tag = "\n</think>\n";
                ctx->fullResponse += tag;
                ctx->callback(tag);
              }
              ctx->fullResponse += token;
              ctx->callback(token);
            }
          }
        } else if (choice.contains("message") && choice["message"].contains("content") && choice["message"]["content"].is_string()) {
          std::string token = choice["message"]["content"].get<std::string>();
          if (!token.empty()) {
            ctx->fullResponse += token;
            ctx->callback(token);
          }
        }
      }
    } catch (const std::exception& e) {
      std::string errStr = "OpenRouter JSON Parse Error: " + std::string(e.what()) + " Line: " + line;
      if (ctx->aiInstance->errorLogger) ctx->aiInstance->errorLogger(errStr);
      else std::cerr << errStr << std::endl;
    }
  }

  return real_size;
}

OpenRouterAI::OpenRouterAI(const std::string &apiKey, const std::string &model)
    : apiKey(apiKey), modelName(model) {}

void OpenRouterAI::setOption(const std::string &key, const nlohmann::json &value) {
  options[key] = value;
}

std::string OpenRouterAI::generate(const std::string &contextId,
                                   const std::vector<ChatMessage> &history,
                                   std::stop_token stoken) {
  {
    std::lock_guard<std::mutex> lock(busyMutex);
    busyContexts.insert(contextId);
  }
  totalRequests++;

  CURL *curl;
  std::string readBuffer;

  curl = curl_easy_init();
  if (curl) {
    std::string endpoint = "https://openrouter.ai/api/v1/chat/completions";
    curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());

    std::vector<nlohmann::json> historyJson;
    for (const auto& msg : history) {
      historyJson.push_back({
        {"role", msg.role},
        {"content", msg.content}
      });
    }

    nlohmann::json payload = {
      {"model", modelName},
      {"messages", historyJson},
      {"stream", false},
      {"include_reasoning", true}
    };
    
    for (auto& el : options.items()) {
      payload[el.key()] = el.value();
    }

    std::string jsonStr = payload.dump();

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, ("Authorization: Bearer " + apiKey).c_str());
    headers = curl_slist_append(headers, "HTTP-Referer: http://localhost:8080");
    
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonStr.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);

    CURLM *multi_handle = curl_multi_init();
    curl_multi_add_handle(multi_handle, curl);

    int still_running = 1;
    while (still_running && !stoken.stop_requested()) {
      curl_multi_perform(multi_handle, &still_running);
      if (still_running) {
        curl_multi_wait(multi_handle, NULL, 0, 10, NULL);
      }
    }

    if (stoken.stop_requested()) {
      curl_multi_remove_handle(multi_handle, curl);
      curl_multi_cleanup(multi_handle);
      curl_slist_free_all(headers);
      curl_easy_cleanup(curl);
      std::lock_guard<std::mutex> lock(busyMutex);
      busyContexts.erase(contextId);
      return "";
    }

    int msgs_left;
    CURLMsg *msg;
    while ((msg = curl_multi_info_read(multi_handle, &msgs_left))) {
      if (msg->msg == CURLMSG_DONE && msg->data.result != CURLE_OK) {
        std::string errStr = "curl_multi failed: " + std::string(curl_easy_strerror(msg->data.result));
        if (errorLogger) errorLogger(errStr);
        else std::cerr << errStr << std::endl;
      }
    }

    curl_multi_remove_handle(multi_handle, curl);
    curl_multi_cleanup(multi_handle);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
  }

  try {
    auto j = nlohmann::json::parse(readBuffer);
    std::string resp = "";
    if (j.contains("choices") && j["choices"].is_array() && !j["choices"].empty()) {
      auto &choice = j["choices"][0];
      if (choice.contains("message")) {
        auto &msg = choice["message"];
        std::string reasoning = "";
        if (msg.contains("reasoning") && msg["reasoning"].is_string()) {
          reasoning = msg["reasoning"].get<std::string>();
        }
        if (msg.contains("content") && msg["content"].is_string()) {
          resp = msg["content"].get<std::string>();
        }
        if (!reasoning.empty()) {
          resp = "<think>\n" + reasoning + "\n</think>\n" + resp;
        }
      }
    }

    if (!resp.empty()) {
      // Local history tracking is now handled by the caller (NPCContext)

      {
        std::lock_guard<std::mutex> lock(messagesMutex);
        lastMessages[contextId] = resp;
      }
    } else if (j.contains("error")) {
        std::string errStr = "OpenRouter API Error: " + j["error"].dump();
        if (errorLogger) errorLogger(errStr);
        else std::cerr << errStr << std::endl;
    }
    
    {
      std::lock_guard<std::mutex> lock(busyMutex);
      busyContexts.erase(contextId);
    }
    
    if (!resp.empty()) return resp;
  } catch (...) {
    std::lock_guard<std::mutex> lock(busyMutex);
    busyContexts.erase(contextId);
  }

  return readBuffer;
}

bool OpenRouterAI::isBusy(const std::string &contextId) {
  std::lock_guard<std::mutex> lock(busyMutex);
  return busyContexts.find(contextId) != busyContexts.end();
}

bool OpenRouterAI::generateStream(const std::string &contextId,
                                  const std::vector<ChatMessage> &history,
                                  StreamCallback callback, std::stop_token stoken) {
  {
    std::lock_guard<std::mutex> lock(busyMutex);
    busyContexts.insert(contextId);
  }
  totalRequests++;

  std::vector<nlohmann::json> historyJson;
  for (const auto& msg : history) {
    historyJson.push_back({
      {"role", msg.role},
      {"content", msg.content}
    });
  }

  nlohmann::json payload = {
    {"model", modelName},
    {"messages", historyJson},
    {"stream", true},
    {"include_reasoning", true}
  };
  
  for (auto& el : options.items()) {
    payload[el.key()] = el.value();
  }

  std::string jsonStr = payload.dump();

  std::thread([this, contextId, callback, stoken, jsonStr]() mutable {
    StreamContext ctx;
    ctx.callback = callback;
    ctx.contextId = contextId;
    ctx.aiInstance = this;

    CURL *curl = curl_easy_init();
    if (!curl) {
      std::lock_guard<std::mutex> lock(busyMutex);
      busyContexts.erase(contextId);
      callback("");
      return;
    }

    std::string endpoint = "https://openrouter.ai/api/v1/chat/completions";
    curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, ("Authorization: Bearer " + apiKey).c_str());
    headers = curl_slist_append(headers, "HTTP-Referer: http://localhost:8080");

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonStr.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallbackStream);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);

    CURLM *multi_handle = curl_multi_init();
    curl_multi_add_handle(multi_handle, curl);

    int still_running = 1;
    while (still_running && !stoken.stop_requested()) {
      curl_multi_perform(multi_handle, &still_running);
      if (still_running) {
        curl_multi_wait(multi_handle, NULL, 0, 10, NULL);
      }
    }

    bool success = false;
    if (!stoken.stop_requested()) {
      int msgs_left;
      CURLMsg *msg;
      while ((msg = curl_multi_info_read(multi_handle, &msgs_left))) {
        if (msg->msg == CURLMSG_DONE) {
          if (msg->data.result == CURLE_OK) {
            success = true;
          } else {
            std::string errStr = "curl stream failed: " + std::string(curl_easy_strerror(msg->data.result));
            if (errorLogger) errorLogger(errStr);
            else std::cerr << errStr << std::endl;
          }
        }
      }
    }

    curl_multi_remove_handle(multi_handle, curl);
    curl_multi_cleanup(multi_handle);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (success && !ctx.fullResponse.empty()) {
      if (ctx.isThinking) {
        ctx.isThinking = false;
        std::string tag = "\n</think>\n";
        ctx.fullResponse += tag;
        callback(tag);
      }
      std::lock_guard<std::mutex> lock(messagesMutex);
      lastMessages[contextId] = ctx.fullResponse;
      fullContexts[contextId] += "\nResponse: " + ctx.fullResponse;
    }

    {
      std::lock_guard<std::mutex> lock(busyMutex);
      busyContexts.erase(contextId);
    }
    
    callback("");
  }).detach();

  return true;
}

std::string OpenRouterAI::getLastMessage(const std::string &contextId) {
  std::lock_guard<std::mutex> lock(messagesMutex);
  auto it = lastMessages.find(contextId);
  if (it != lastMessages.end()) {
    return it->second;
  }
  return "";
}

std::string OpenRouterAI::getContext(const std::string &contextId) {
  std::lock_guard<std::mutex> lock(messagesMutex);
  auto it = fullContexts.find(contextId);
  if (it != fullContexts.end()) {
    return it->second;
  }
  return "";
}
