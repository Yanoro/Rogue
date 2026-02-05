#include "AI.h"
#include <curl/curl.h>
#include <iostream>

size_t OllamaAI::WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
  ((std::string*)userp)->append((char*)contents, size * nmemb);
  return size * nmemb;
}

size_t OllamaAI::WriteCallbackStream(void* contents, size_t size, size_t nmemb, void* userp) {
  size_t real_size = size * nmemb;
  StreamContext* ctx = (StreamContext*)userp;

  ctx->buffer.append((char*)contents, real_size);

  size_t pos = 0;
  while ((pos = ctx->buffer.find('\n')) != std::string::npos) {
    std::string line = ctx->buffer.substr(0, pos);
    ctx->buffer.erase(0, pos + 1);

    if (line.empty()) continue;

    try {
      auto j = nlohmann::json::parse(line);
      if (j.contains("response") && !j["response"].is_null()) {
        std::string token = j["response"];
        ctx->callback(token);
      }

    } 
    catch (const std::exception& e) {
      std::cerr << "JSON Parse Error: " << e.what() << " Line: " << line << std::endl;
    }
  }

  return real_size;
}

OllamaAI::OllamaAI(const std::string& model, const std::string& url) 
: modelName(model), endpoint(url) {}

void OllamaAI::setOption(const std::string& key, const nlohmann::json& value) {
  options[key] = value;
}

std::string OllamaAI::generate(const std::string& prompt) {
  CURL* curl;
  CURLcode res;
  std::string readBuffer;

  curl = curl_easy_init();
  if(curl) {
    curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());

    nlohmann::json payload = {
      {"model", modelName},
      {"prompt", prompt},
      {"stream", false} 
    };

    if (!options.empty()) {
      payload["options"] = options;
    }

    std::string jsonStr = payload.dump();

    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonStr.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);

    res = curl_easy_perform(curl);

    if(res != CURLE_OK) {
      std::cerr << "curl_easy_perform() failed: " << curl_easy_strerror(res) << std::endl;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
  }

  try {
    auto j = nlohmann::json::parse(readBuffer);
    if (j.contains("response")) {
      return j["response"];
    }
  } catch (...) {}

  return readBuffer; // Return raw if parse fails or no response field
}

void OllamaAI::generateStream(const std::string& prompt, StreamCallback callback) {
  CURL* curl;
  CURLcode res;
  StreamContext ctx;
  ctx.callback = callback;

  curl = curl_easy_init();
  if(curl) {
    curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());

    nlohmann::json payload = {
      {"model", modelName},
      {"prompt", prompt},
      {"stream", true},
    };

    if (!options.empty()) {
      payload["options"] = options;
    }

    if (!context.empty()) {
      payload["context"] = context;
    }

    std::string jsonStr = payload.dump();

    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonStr.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallbackStream);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);

    res = curl_easy_perform(curl);

    if(res != CURLE_OK) {
      std::cerr << "curl_easy_perform() failed: " << curl_easy_strerror(res) << std::endl;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
  }
}
