#include "AI.h"
#include <curl/curl.h>
#include <iostream>
#include <thread>

size_t OllamaAI::WriteCallback(void *contents, size_t size, size_t nmemb,
                               void *userp) {
  ((std::string *)userp)->append((char *)contents, size * nmemb);
  return size * nmemb;
}

size_t OllamaAI::WriteCallbackStream(void *contents, size_t size, size_t nmemb,
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

    try {
      auto j = nlohmann::json::parse(line);
      if (j.contains("response") && !j["response"].is_null()) {
        std::string token = j["response"];
        ctx->fullResponse += token;
        ctx->callback(token);
      }

      if (j.contains("done") && j["done"] == true) {
        if (j.contains("context")) {
          ctx->aiInstance->contexts[ctx->contextId] =
              j["context"].get<std::vector<int>>();
        }
      }

    } catch (const std::exception &e) {
      std::cerr << "JSON Parse Error: " << e.what() << " Line: " << line
                << std::endl;
    }
  }

  return real_size;
}

OllamaAI::OllamaAI(const std::string &model, const std::string &url)
    : modelName(model), endpoint(url) {}

void OllamaAI::setOption(const std::string &key, const nlohmann::json &value) {
  options[key] = value;
}

std::string OllamaAI::generate(const std::string &contextId,
                               const std::string &prompt,
                               std::stop_token stoken) {
  {
    std::lock_guard<std::mutex> lock(busyMutex);
    busyContexts.insert(contextId);
  }

  CURL *curl;
  std::string readBuffer;

  curl = curl_easy_init();
  if (curl) {
    curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());

    nlohmann::json payload = {
        {"model", modelName}, {"prompt", prompt}, {"stream", false}};

    if (!options.empty()) {
      payload["options"] = options;
    }

    if (contexts.find(contextId) != contexts.end() &&
        !contexts[contextId].empty()) {
      payload["context"] = contexts[contextId];
    }

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
    if (j.contains("done") && j["done"] == true) {
      if (j.contains("context")) {
        contexts[contextId] = j["context"].get<std::vector<int>>();
      }
    }

    {
      std::lock_guard<std::mutex> lock(busyMutex);
      busyContexts.erase(contextId);
    }

    if (j.contains("response")) {
      std::string resp = j["response"];
      {
        std::lock_guard<std::mutex> lock(messagesMutex);
        lastMessages[contextId] = resp;
        fullContexts[contextId] +=
            "\nPrompt: " + prompt + "\nResponse: " + resp;
      }
      return resp;
    }
  } catch (...) {
    std::lock_guard<std::mutex> lock(busyMutex);
    busyContexts.erase(contextId);
  }

  return readBuffer; // Return raw if parse fails or no response field
}

bool OllamaAI::isBusy(const std::string &contextId) {
  std::lock_guard<std::mutex> lock(busyMutex);
  return busyContexts.find(contextId) != busyContexts.end();
}

bool OllamaAI::generateStream(const std::string &contextId,
                              const std::string &prompt,
                              StreamCallback callback, std::stop_token stoken) {
  {
    std::lock_guard<std::mutex> lock(busyMutex);
    busyContexts.insert(contextId);
  }

  nlohmann::json payload = {
      {"model", modelName},
      {"prompt", prompt},
      {"stream", true},
  };

  if (!options.empty()) {
    payload["options"] = options;
  }

  if (contexts.find(contextId) != contexts.end() &&
      !contexts[contextId].empty()) {
    payload["context"] = contexts[contextId];
  }

  std::string jsonStr = payload.dump();

  std::thread([this, contextId, prompt, callback, stoken, jsonStr]() {
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

    if (success) {
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

std::string OllamaAI::getLastMessage(const std::string &contextId) {
  std::lock_guard<std::mutex> lock(messagesMutex);
  auto it = lastMessages.find(contextId);
  if (it != lastMessages.end()) {
    return it->second;
  }
  return "";
}

std::string OllamaAI::getContext(const std::string &contextId) {
  std::lock_guard<std::mutex> lock(messagesMutex);
  auto it = fullContexts.find(contextId);
  if (it != fullContexts.end()) {
    return it->second;
  }
  return "";
}
