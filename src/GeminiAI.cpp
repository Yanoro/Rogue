#include "GeminiAI.h"
#include <curl/curl.h>
#include <iostream>
#include <thread>

size_t GeminiAI::WriteCallback(void *contents, size_t size, size_t nmemb,
                               void *userp) {
  ((std::string *)userp)->append((char *)contents, size * nmemb);
  return size * nmemb;
}

size_t GeminiAI::WriteCallbackStream(void *contents, size_t size, size_t nmemb,
                                     void *userp) {
  size_t real_size = size * nmemb;
  StreamContext *ctx = (StreamContext *)userp;

  ctx->buffer.append((char *)contents, real_size);

  size_t pos = 0;
  while ((pos = ctx->buffer.find('\n')) != std::string::npos) {
    std::string line = ctx->buffer.substr(0, pos);
    ctx->buffer.erase(0, pos + 1);

    if (line.empty())
      continue;

    if (line.rfind("data: ", 0) == 0) {
      line = line.substr(6);
    }
    
    try {
      auto j = nlohmann::json::parse(line);
      if (j.contains("candidates") && j["candidates"].is_array() && !j["candidates"].empty()) {
        auto &candidate = j["candidates"][0];
        if (candidate.contains("content") && candidate["content"].contains("parts")) {
          auto &parts = candidate["content"]["parts"];
          if (parts.is_array() && !parts.empty()) {
            std::string token = parts[0]["text"];
            ctx->fullResponse += token;
            ctx->callback(token);
          }
        }
      }
    } catch (...) {
    }
  }

  return real_size;
}

GeminiAI::GeminiAI(const std::string &apiKey, const std::string &model)
    : apiKey(apiKey), modelName(model) {}

std::string GeminiAI::generate(const std::string &contextId,
                               const std::vector<ChatMessage> &history,
                               std::stop_token stoken) {
  std::string prompt = "";
  for (const auto& msg : history) {
    if (msg.role == "assistant") prompt += "You: " + msg.content + "\n";
    else prompt += msg.content + "\n";
  }
  {
    std::lock_guard<std::mutex> lock(busyMutex);
    busyContexts.insert(contextId);
  }

  CURL *curl;
  std::string readBuffer;

  curl = curl_easy_init();
  if (curl) {
    std::string endpoint = "https://generativelanguage.googleapis.com/v1beta/models/" + modelName + ":generateContent?key=" + apiKey;
    curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());

    std::vector<nlohmann::json> history;
    if (contexts.find(contextId) != contexts.end()) {
      history = contexts[contextId];
    }
    history.push_back({
      {"role", "user"},
      {"parts", {{{"text", prompt}}}}
    });

    nlohmann::json payload = {{"contents", history}};
    std::string jsonStr = payload.dump();

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
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
        std::cerr << "curl_multi failed: "
                  << curl_easy_strerror(msg->data.result) << std::endl;
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
    if (j.contains("candidates") && j["candidates"].is_array() && !j["candidates"].empty()) {
      auto &candidate = j["candidates"][0];
      if (candidate.contains("content") && candidate["content"].contains("parts")) {
        auto &parts = candidate["content"]["parts"];
        if (parts.is_array() && !parts.empty()) {
          resp = parts[0]["text"];
        }
      }
    }

    if (!resp.empty()) {
      std::vector<nlohmann::json> history;
      if (contexts.find(contextId) != contexts.end()) {
        history = contexts[contextId];
      }
      history.push_back({
        {"role", "user"},
        {"parts", {{{"text", prompt}}}}
      });
      // Local history tracking is handled by NPCContext
      contexts[contextId] = history;

      {
        std::lock_guard<std::mutex> lock(messagesMutex);
        lastMessages[contextId] = resp;
        fullContexts[contextId] +=
            "\nPrompt: " + prompt + "\nResponse: " + resp;
      }
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

bool GeminiAI::isBusy(const std::string &contextId) {
  std::lock_guard<std::mutex> lock(busyMutex);
  return busyContexts.find(contextId) != busyContexts.end();
}

bool GeminiAI::generateStream(const std::string &contextId,
                              const std::vector<ChatMessage> &history,
                              StreamCallback callback, std::stop_token stoken) {
  std::string prompt = "";
  for (const auto& msg : history) {
    if (msg.role == "assistant") prompt += "You: " + msg.content + "\n";
    else prompt += msg.content + "\n";
  }
  {
    std::lock_guard<std::mutex> lock(busyMutex);
    busyContexts.insert(contextId);
  }

  std::vector<nlohmann::json> historyJson;
  if (contexts.find(contextId) != contexts.end()) {
    historyJson = contexts[contextId];
  }
  historyJson.push_back({
    {"role", "user"},
    {"parts", {{{"text", prompt}}}}
  });

  nlohmann::json payload = {{"contents", historyJson}};
  std::string jsonStr = payload.dump();

  std::thread([this, contextId, prompt, history, callback, stoken, jsonStr]() mutable {
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

    std::string endpoint = "https://generativelanguage.googleapis.com/v1beta/models/" + modelName + ":streamGenerateContent?alt=sse&key=" + apiKey;
    curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
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
            std::cerr << "curl stream failed: "
                      << curl_easy_strerror(msg->data.result) << std::endl;
          }
        }
      }
    }

    curl_multi_remove_handle(multi_handle, curl);
    curl_multi_cleanup(multi_handle);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (success && !ctx.fullResponse.empty()) {
      // Local history tracking is handled by NPCContext

      std::lock_guard<std::mutex> lock(messagesMutex);
      lastMessages[contextId] = ctx.fullResponse;
      fullContexts[contextId] +=
          "\nPrompt: " + prompt + "\nResponse: " + ctx.fullResponse;
    }

    {
      std::lock_guard<std::mutex> lock(busyMutex);
      busyContexts.erase(contextId);
    }
    
    callback("");
  }).detach();

  return true;
}

std::string GeminiAI::getLastMessage(const std::string &contextId) {
  std::lock_guard<std::mutex> lock(messagesMutex);
  auto it = lastMessages.find(contextId);
  if (it != lastMessages.end()) {
    return it->second;
  }
  return "";
}

std::string GeminiAI::getContext(const std::string &contextId) {
  std::lock_guard<std::mutex> lock(messagesMutex);
  auto it = fullContexts.find(contextId);
  if (it != fullContexts.end()) {
    return it->second;
  }
  return "";
}
