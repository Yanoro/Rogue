#include "OpenRouterAI.h"
#include <curl/curl.h>
#include <algorithm>
#include <cctype>
#include <iostream>
#include <thread>

static std::string FormatMetadataText(const nlohmann::json& j, int indent = 0) {
    std::string result;
    std::string indentStr(indent, ' ');
    if (j.is_object()) {
        for (auto& el : j.items()) {
            if (el.value().is_primitive() && !el.value().is_null()) {
                std::string valStr = el.value().is_string() ? el.value().get<std::string>() : el.value().dump();
                result += indentStr + el.key() + ": " + valStr + "\n";
            } else if (!el.value().is_null()) {
                if (el.value().is_array() && el.value().empty()) {
                   result += indentStr + el.key() + ": []\n";
                } else if (el.value().is_object() && el.value().empty()) {
                   result += indentStr + el.key() + ": {}\n";
                } else {
                   result += indentStr + el.key() + ":\n" + FormatMetadataText(el.value(), indent + 2);
                }
            }
        }
    } else if (j.is_array()) {
        for (size_t i = 0; i < j.size(); ++i) {
            if (j[i].is_primitive()) {
               std::string valStr = j[i].is_string() ? j[i].get<std::string>() : j[i].dump();
               result += indentStr + "- " + valStr + "\n";
            } else {
               result += indentStr + "- \n" + FormatMetadataText(j[i], indent + 2);
            }
        }
    } else if (j.is_primitive() && !j.is_null()) {
        std::string valStr = j.is_string() ? j.get<std::string>() : j.dump();
        result += indentStr + valStr + "\n";
    }
    return result;
}

static std::string ToLower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return text;
}

// Reasoning budget requested per turn. Reasoning tokens count against the
// request's max_tokens, so this is capped to leave room for the visible reply.
static constexpr int DEFAULT_REASONING_MAX_TOKENS = 512;

// Reasoning text arrives either in the legacy `reasoning` string or in the
// normalized `reasoning_details` array; some providers populate only one.
// `reasoning_details` holds reasoning.text / reasoning.summary entries
// (encrypted blocks carry no readable text). When both fields are present they
// describe the same text, so prefer one instead of concatenating both.
static std::string ExtractReasoningText(const nlohmann::json &delta) {
  for (const char *key : {"reasoning", "reasoning_content"}) {
    if (delta.contains(key) && delta[key].is_string()) {
      std::string text = delta[key].get<std::string>();
      if (!text.empty()) return text;
    }
  }

  std::string text;
  if (delta.contains("reasoning_details") && delta["reasoning_details"].is_array()) {
    for (const auto &detail : delta["reasoning_details"]) {
      if (!detail.is_object()) continue;
      if (detail.contains("text") && detail["text"].is_string()) {
        text += detail["text"].get<std::string>();
      } else if (detail.contains("summary") && detail["summary"].is_string()) {
        text += detail["summary"].get<std::string>();
      }
    }
  }
  return text;
}

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
      {
        std::lock_guard<std::mutex> lock(ctx->aiInstance->metadataMutex);
        if (j.contains("model") && j["model"].is_string()) {
          std::string modelStr = j["model"].get<std::string>();
          size_t slashPos = modelStr.find('/');
          if (slashPos != std::string::npos) {
              ctx->aiInstance->currentProvider = modelStr.substr(0, slashPos);
          }
        }
        if (j.contains("openrouter_metadata") && j["openrouter_metadata"].is_object()) {
          auto& metadata = j["openrouter_metadata"];
          ctx->aiInstance->rawMetadata = FormatMetadataText(metadata);
          if (metadata.contains("endpoints") && metadata["endpoints"].contains("available") && metadata["endpoints"]["available"].is_array() && !metadata["endpoints"]["available"].empty()) {
              auto& endpoint = metadata["endpoints"]["available"][0];
              if (endpoint.contains("provider") && endpoint["provider"].is_string()) {
                  ctx->aiInstance->currentProvider = endpoint["provider"].get<std::string>();
              }
              if (endpoint.contains("quantization") && endpoint["quantization"].is_string()) {
                  ctx->aiInstance->currentQuantization = endpoint["quantization"].get<std::string>();
              }
          }
        }
      }
      if (j.contains("error")) {
        std::string errStr = "OpenRouter Stream Error: " + j["error"].dump();
        if (ctx->aiInstance->errorLogger) ctx->aiInstance->errorLogger(errStr);
        else std::cerr << errStr << std::endl;
      } else if (j.contains("choices") && j["choices"].is_array() && !j["choices"].empty()) {
        auto &choice = j["choices"][0];
        if (choice.contains("finish_reason") && choice["finish_reason"].is_string() &&
            choice["finish_reason"].get<std::string>() == "length") {
          if (ctx->aiInstance->errorLogger) {
            ctx->aiInstance->errorLogger(
                "OpenRouter: response truncated (finish_reason=length) - reasoning "
                "tokens may have consumed the max_tokens budget");
          }
        }
        if (choice.contains("delta")) {
          auto &delta = choice["delta"];
          std::string reasoningToken = ExtractReasoningText(delta);
          if (!reasoningToken.empty()) {
            if (!ctx->isThinking) {
              ctx->isThinking = true;
              std::string tag = "<think>\n";
              ctx->fullResponse += tag;
              ctx->callback(tag);
            }
            ctx->fullResponse += reasoningToken;
            ctx->callback(reasoningToken);
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

// Quantization is NOT part of the per-request `openrouter_metadata`; that block
// only lists provider/model/selected per candidate endpoint. The real value
// lives on each entry of GET /api/v1/models/{author}/{slug}/endpoints.
void OpenRouterAI::fetchEndpointMetadata() {
  CURL *curl = curl_easy_init();
  if (!curl) {
    return;
  }

  std::string url =
      "https://openrouter.ai/api/v1/models/" + modelName + "/endpoints";
  std::string readBuffer;

  struct curl_slist *headers = NULL;
  headers = curl_slist_append(headers, ("Authorization: Bearer " + apiKey).c_str());
  headers = curl_slist_append(headers, "HTTP-Referer: http://localhost:8080");

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

  CURLcode res = curl_easy_perform(curl);

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK) {
    std::string errStr = "Failed to fetch OpenRouter endpoint metadata: " +
                         std::string(curl_easy_strerror(res));
    if (errorLogger) errorLogger(errStr);
    else std::cerr << errStr << std::endl;
    return;
  }

  try {
    auto j = nlohmann::json::parse(readBuffer);
    if (!j.contains("data") || !j["data"].is_object()) {
      return;
    }
    auto &data = j["data"];
    if (!data.contains("endpoints") || !data["endpoints"].is_array()) {
      return;
    }

    std::lock_guard<std::mutex> lock(metadataMutex);
    for (auto &endpoint : data["endpoints"]) {
      if (!endpoint.is_object()) continue;
      if (!endpoint.contains("provider_name") || !endpoint["provider_name"].is_string()) continue;
      if (!endpoint.contains("quantization") || !endpoint["quantization"].is_string()) continue;
      quantizationByProvider[ToLower(endpoint["provider_name"].get<std::string>())] =
          endpoint["quantization"].get<std::string>();
    }
  } catch (const std::exception &e) {
    std::string errStr =
        "Failed to parse OpenRouter endpoint metadata: " + std::string(e.what());
    if (errorLogger) errorLogger(errStr);
    else std::cerr << errStr << std::endl;
  }
}

std::string OpenRouterAI::getQuantization() const {
  std::lock_guard<std::mutex> lock(metadataMutex);

  // Value delivered inline with a response, if OpenRouter ever adds one.
  if (!currentQuantization.empty() && currentQuantization != "Unknown") {
    return currentQuantization;
  }

  auto it = quantizationByProvider.find(ToLower(currentProvider));
  if (it != quantizationByProvider.end() && !it->second.empty()) {
    return it->second;
  }

  // Router metadata is absent on cache hits, so currentProvider may still be
  // the model author rather than the serving provider. One candidate is
  // unambiguous; several are not, so prefer "Unknown" over a guess.
  if (quantizationByProvider.size() == 1) {
    return quantizationByProvider.begin()->second;
  }

  return "Unknown";
}

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
  long httpCode = 0;

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
      // `exclude` only controls whether reasoning is returned; `enabled` is what
      // turns it on. The legacy `include_reasoning` flag is equivalent to an
      // empty `reasoning` object and does not enable reasoning by itself.
      {"reasoning", {
        {"enabled", true},
        {"exclude", false},
        {"max_tokens", DEFAULT_REASONING_MAX_TOKENS}
      }}
    };
    
    for (auto& el : options.items()) {
      payload[el.key()] = el.value();
    }

    std::string jsonStr = payload.dump();

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, ("Authorization: Bearer " + apiKey).c_str());
    headers = curl_slist_append(headers, "HTTP-Referer: http://localhost:8080");
    headers = curl_slist_append(headers, "X-OpenRouter-Metadata: enabled");
    
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

    // curl treats HTTP 4xx/5xx as a successful transfer, so the status has to be
    // read explicitly or a rate-limit body looks like an empty response.
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

    curl_multi_remove_handle(multi_handle, curl);
    curl_multi_cleanup(multi_handle);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
  }

  if (httpCode >= 400) {
    std::string detail = readBuffer.substr(0, 400);
    std::string errStr =
        "OpenRouter request failed: HTTP " + std::to_string(httpCode);
    if (!detail.empty()) {
      errStr += " - " + detail;
    }
    if (errorLogger) errorLogger(errStr);
    else std::cerr << errStr << std::endl;

    std::lock_guard<std::mutex> lock(busyMutex);
    busyContexts.erase(contextId);
    return "";
  }

  try {
    auto j = nlohmann::json::parse(readBuffer);

    {
      std::lock_guard<std::mutex> lock(metadataMutex);
      if (j.contains("model") && j["model"].is_string()) {
          std::string modelStr = j["model"].get<std::string>();
          size_t slashPos = modelStr.find('/');
          if (slashPos != std::string::npos) {
              currentProvider = modelStr.substr(0, slashPos);
          }
      }
      if (j.contains("openrouter_metadata") && j["openrouter_metadata"].is_object()) {
        auto& metadata = j["openrouter_metadata"];
        rawMetadata = FormatMetadataText(metadata);
        if (metadata.contains("endpoints") && metadata["endpoints"].contains("available") && metadata["endpoints"]["available"].is_array() && !metadata["endpoints"]["available"].empty()) {
            auto& endpoint = metadata["endpoints"]["available"][0];
            if (endpoint.contains("provider") && endpoint["provider"].is_string()) {
                currentProvider = endpoint["provider"].get<std::string>();
            }
            if (endpoint.contains("quantization") && endpoint["quantization"].is_string()) {
                currentQuantization = endpoint["quantization"].get<std::string>();
            }
        }
      }
    }

    std::string resp = "";
    if (j.contains("choices") && j["choices"].is_array() && !j["choices"].empty()) {
      auto &choice = j["choices"][0];
      if (choice.contains("finish_reason") && choice["finish_reason"].is_string() &&
          choice["finish_reason"].get<std::string>() == "length") {
        std::string errStr =
            "OpenRouter: response truncated (finish_reason=length) - reasoning "
            "tokens may have consumed the max_tokens budget";
        if (errorLogger) errorLogger(errStr);
        else std::cerr << errStr << std::endl;
      }
      if (choice.contains("message")) {
        auto &msg = choice["message"];
        std::string reasoning = ExtractReasoningText(msg);
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
    // `exclude` only controls whether reasoning is returned; `enabled` is what
    // turns it on. The legacy `include_reasoning` flag is equivalent to an
    // empty `reasoning` object and does not enable reasoning by itself.
    {"reasoning", {
      {"enabled", true},
      {"exclude", false},
      {"max_tokens", DEFAULT_REASONING_MAX_TOKENS}
    }}
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
    headers = curl_slist_append(headers, "X-OpenRouter-Metadata: enabled");

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

    // curl reports HTTP 4xx/5xx as a successful transfer. Without this check a
    // rate-limited request (e.g. 429 "engine_overloaded") silently yields an
    // empty response, which the game then parses as an invalid command.
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    if (httpCode >= 400) {
      std::string detail = ctx.buffer;
      if (detail.size() > 400) detail = detail.substr(0, 400);
      std::string errStr =
          "OpenRouter stream failed: HTTP " + std::to_string(httpCode);
      if (!detail.empty()) errStr += " - " + detail;
      if (errorLogger) errorLogger(errStr);
      else std::cerr << errStr << std::endl;
      success = false;
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

    // Resolve per-provider quantization once, off the main thread. It is not
    // part of the streamed response metadata.
    if (!endpointsFetched.exchange(true)) {
      fetchEndpointMetadata();
    }
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
