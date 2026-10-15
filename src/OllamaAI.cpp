#include "OllamaAI.h"
#include <curl/curl.h>
#include <iostream>
#include <thread>

namespace {

// Ollama reports durations as integer nanoseconds and counts as integers; these
// tolerate a missing or oddly-typed field by yielding zero rather than throwing,
// because a broken timing field must never fail the request that carried it.
// See https://docs.ollama.com/api/generate for the field list.
double JsonDurationSeconds(const nlohmann::json &j, const char *key) {
  if (!j.contains(key) || !j[key].is_number()) {
    return 0.0;
  }
  return static_cast<double>(j[key].get<long long>()) / 1000000000.0;
}

int JsonCount(const nlohmann::json &j, const char *key) {
  if (!j.contains(key) || !j[key].is_number()) {
    return 0;
  }
  return j[key].get<int>();
}

} // namespace

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

      // The `done` branch runs *before* the token dispatch below, and that order
      // is load-bearing. Ollama's final chunk carries the timing fields and an
      // empty "response" in the same object, and an empty token is the
      // end-of-stream sentinel every caller keys on -- so a caller that reacts to
      // the sentinel by reading the stats would race this line and see the
      // previous request's numbers if the stats were recorded second.
      if (j.contains("done") && j["done"] == true) {
        if (j.contains("context")) {
          ctx->aiInstance->contexts[ctx->contextId] =
              j["context"].get<std::vector<int>>();
        }

        AIRequestStats stats;
        stats.valid = true;
        stats.promptTokens = JsonCount(j, "prompt_eval_count");
        stats.outputTokens = JsonCount(j, "eval_count");
        stats.loadSeconds = JsonDurationSeconds(j, "load_duration");
        stats.promptEvalSeconds = JsonDurationSeconds(j, "prompt_eval_duration");
        stats.evalSeconds = JsonDurationSeconds(j, "eval_duration");
        stats.totalSeconds = JsonDurationSeconds(j, "total_duration");

        std::lock_guard<std::mutex> lock(ctx->aiInstance->statsMutex);
        ctx->aiInstance->lastStats[ctx->contextId] = stats;
      }

      if (j.contains("response") && !j["response"].is_null()) {
        std::string token = j["response"];
        ctx->fullResponse += token;
        ctx->callback(token);
      }

    } catch (const std::exception &e) {
      std::string errStr = "JSON Parse Error: " + std::string(e.what()) + " Line: " + line;
      if (ctx->aiInstance->errorLogger) ctx->aiInstance->errorLogger(errStr);
      else std::cerr << errStr << std::endl;
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
                               const std::vector<ChatMessage> &history,
                               std::stop_token stoken,
                               const ImageList &images) {
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
    curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());

    nlohmann::json payload = {
        {"model", modelName}, {"prompt", prompt}, {"stream", false}};

    // How long to keep the model resident. Sending nothing leaves Ollama's own
    // default, which unloads after 5 idle minutes -- and reloading a multi-GB
    // model costs far more than the request itself, which is exactly the kind of
    // one-off delay that reads as "the model got slower".
    if (!keepAlive.empty()) {
      payload["keep_alive"] = keepAlive;
    }

    // Ollama takes images as a sibling of "prompt": an array of bare base64
    // strings, one per image. Omitted entirely when empty so a text-only request
    // is byte-for-byte what it was before.
    if (!images.empty()) {
      payload["images"] = images;
    }

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
    // Local history tracking is handled by NPCContext

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
                              const std::vector<ChatMessage> &history,
                              StreamCallback callback, std::stop_token stoken,
                              const ImageList &images) {
  std::string prompt = "";
  for (const auto& msg : history) {
    if (msg.role == "assistant") prompt += "You: " + msg.content + "\n";
    else prompt += msg.content + "\n";
  }
  {
    std::lock_guard<std::mutex> lock(busyMutex);
    busyContexts.insert(contextId);
  }

  nlohmann::json payload = {
      {"model", modelName},
      {"prompt", prompt},
      {"stream", true},
  };

  // See generate() for why this is sent at all.
  if (!keepAlive.empty()) {
    payload["keep_alive"] = keepAlive;
  }

  // See generate(): bare base64 strings next to "prompt", omitted when there are
  // none. The images travel inside `jsonStr`, which the worker thread captures by
  // value, so the caller does not have to keep them alive.
  if (!images.empty()) {
    payload["images"] = images;
  }

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

void OllamaAI::resetContext(const std::string &contextId) {
  // `contexts` has no mutex of its own -- the streaming worker writes it while
  // parsing the response (see WriteCallbackStream) and the request path reads it
  // -- which is safe here only because the caller is required not to reset while
  // a request for this context is in flight.
  contexts.erase(contextId);

  {
    // Dropped with the context: a caller that resets and then asks for stats
    // would otherwise be handed the previous conversation's timings.
    std::lock_guard<std::mutex> lock(statsMutex);
    lastStats.erase(contextId);
  }

  std::lock_guard<std::mutex> lock(messagesMutex);
  lastMessages.erase(contextId);
  fullContexts.erase(contextId);
}

bool OllamaAI::getLastStats(const std::string &contextId, AIRequestStats &out) {
  std::lock_guard<std::mutex> lock(statsMutex);
  auto it = lastStats.find(contextId);
  if (it == lastStats.end()) {
    return false;
  }
  out = it->second;
  return out.valid;
}
