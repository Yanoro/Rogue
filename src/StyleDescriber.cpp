#include "StyleDescriber.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "AI.h"
#include "DebugLog.h"
#include "TileDesigner.hpp"

namespace StyleDescriber {

namespace {

// Long enough for a cold local vision model to load. Ollama's own default keeps a
// model 5 minutes from the last *token*; the load itself is disk I/O on a multi-GB
// file, which is the slow case this budget is sized for.
constexpr int kTimeoutMs = 180000;

// Caps on what a model can put into the paragraph that reaches the document turn.
// A style brief is one short paragraph of context, not an essay: an unbounded reply
// would push the DSL grammar out of the prompt's useful window, and the grammar is
// the part that must not be crowded out.
constexpr size_t kMaxListItem = 48;
constexpr size_t kMaxListSize = 5;
constexpr size_t kMaxFieldText = 160;

std::string Trim(const std::string &text) {
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
    ++begin;
  }
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
    --end;
  }
  return text.substr(begin, end - begin);
}

std::string Join(const std::vector<std::string> &parts) {
  std::string out;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i > 0) out += (i + 1 == parts.size()) ? " and " : ", ";
    out += parts[i];
  }
  return out;
}

// The fallback instructions, used only when the prompt file cannot be read. Short on
// purpose: a missing file must degrade the describer, not silence it -- the same
// bargain BuildSystemPrompt makes for the other designers.
constexpr const char *kFallbackInstructions =
    "You are shown a crop of a pixel-art building. Amber squares with a \"?\" are "
    "parts the world does not have yet. Describe only the surrounding art -- its "
    "materials, tones, light, density and edge treatment -- never the gaps and never "
    "what should be drawn. Reply with prose and exactly one fenced json block holding "
    "materials, tones, light, density and edges.";

}  // namespace

Style ParseReply(const std::string &reply) {
  Style style;
  style.raw = reply;

  // The fence parser is the designers' own, deliberately: it is the third caller that
  // TileDesigner.hpp's comment said would justify sharing it, and a second fence
  // grammar would be a second thing able to disagree about what a reply is.
  const std::string block = TileDesigner::ExtractJsonBlock(reply);
  if (block.empty()) {
    style.error = "the reply had no ```json block";
    return style;
  }

  nlohmann::json parsed;
  try {
    parsed = nlohmann::json::parse(block);
  } catch (const std::exception &error) {
    style.error = std::string("the json block is not valid JSON: ") + error.what();
    return style;
  }
  if (!parsed.is_object()) {
    style.error = "the json block must be an object";
    return style;
  }

  const auto readList = [&parsed](const char *key, std::vector<std::string> &out) {
    const auto entry = parsed.find(key);
    if (entry == parsed.end() || !entry->is_array()) return;
    for (const nlohmann::json &item : *entry) {
      if (out.size() >= kMaxListSize) break;
      if (!item.is_string()) continue;
      std::string text = Trim(item.get<std::string>());
      if (text.empty()) continue;
      if (text.size() > kMaxListItem) {
        text.resize(kMaxListItem);
      }
      out.push_back(std::move(text));
    }
  };
  const auto readText = [&parsed](const char *key, std::string &out) {
    const auto entry = parsed.find(key);
    if (entry == parsed.end() || !entry->is_string()) return;
    out = Trim(entry->get<std::string>());
    if (out.size() > kMaxFieldText) {
      out.resize(kMaxFieldText);
    }
  };

  readList("materials", style.materials);
  readList("tones", style.tones);
  readText("light", style.light);
  readText("density", style.density);
  readText("edges", style.edges);

  if (style.materials.empty() && style.tones.empty() && style.light.empty() &&
      style.density.empty() && style.edges.empty()) {
    style.error =
        "the json block carried none of materials, tones, light, density or edges";
    return style;
  }

  std::ostringstream summary;
  if (!style.materials.empty()) {
    summary << "The surrounding art is made of " << Join(style.materials) << ".";
  }
  if (!style.tones.empty()) {
    summary << " Its tones are " << Join(style.tones) << ".";
  }
  if (!style.light.empty()) {
    summary << " Light and shading: " << style.light << ".";
  }
  if (!style.density.empty()) {
    summary << " Detail density: " << style.density << ".";
  }
  if (!style.edges.empty()) {
    summary << " Edges: " << style.edges << ".";
  }
  summary << " Match that, and do not draw the marked gaps.";
  style.summary = summary.str();
  style.ok = true;
  return style;
}

Job::~Job() {
  {
    std::lock_guard<std::mutex> lock(mutex);
    cancelled = true;
  }
  finished.notify_all();
  if (worker.joinable()) {
    worker.join();
  }
}

void Job::Configure(AI *backend, DebugLog *debugLog) {
  ai = backend;
  log = debugLog;

  instructions.clear();
  std::ifstream file(kDefaultPromptPath);
  if (file.is_open()) {
    std::stringstream buffer;
    buffer << file.rdbuf();
    instructions = buffer.str();
  }
  if (Trim(instructions).empty()) {
    instructions = kFallbackInstructions;
    if (log != nullptr) {
      log->LogWarning(std::string("Style describer: could not read ") +
                      kDefaultPromptPath +
                      "; using the built-in instructions instead.");
    }
  }
}

bool Job::Busy() const {
  std::lock_guard<std::mutex> lock(mutex);
  return busy;
}

bool Job::Request(const std::string &imagePngBase64, const std::string &context,
                  std::string *reason) {
  const auto refuse = [reason](const std::string &text) {
    if (reason != nullptr) *reason = text;
    return false;
  };

  if (ai == nullptr) return refuse("no vision backend is configured");
  if (imagePngBase64.empty()) return refuse("the snapshot produced no image bytes");

  {
    std::lock_guard<std::mutex> lock(mutex);
    if (busy) return refuse("a style request is already in flight");
    busy = true;
    hasResult = false;
    cancelled = false;
    reply.clear();
    streamDone = false;
    transportError.clear();
    result = Style{};
  }

  // A previous worker (if any) has already stored its result and is finishing; joining
  // it here is what keeps `worker` assignable and keeps two vision calls from sharing
  // one context id.
  if (worker.joinable()) {
    worker.join();
  }
  worker = std::thread([this, imagePngBase64, context]() {
    Run(imagePngBase64, context);
  });
  return true;
}

bool Job::Poll(Style *out) {
  if (out == nullptr) return false;
  std::lock_guard<std::mutex> lock(mutex);
  if (!hasResult) return false;
  *out = std::move(result);
  result = Style{};
  hasResult = false;
  return true;
}

bool Job::WaitFor(Style *out, int timeoutMs) {
  if (out == nullptr) return false;
  std::unique_lock<std::mutex> lock(mutex);
  const bool arrived = finished.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                         [this]() { return hasResult; });
  if (!arrived || !hasResult) {
    return false;
  }
  *out = std::move(result);
  result = Style{};
  hasResult = false;
  return true;
}

void Job::Cancel() {
  {
    std::lock_guard<std::mutex> lock(mutex);
    cancelled = true;
  }
  finished.notify_all();
}

void Job::Run(std::string base64, std::string context) {
  // A fresh provider context per request: the history is sent explicitly, and leaving
  // the previous crop's tokens in place would have the model describing two buildings
  // at once.
  ai->resetContext(contextId);

  std::vector<ChatMessage> messages;
  messages.push_back({"system", instructions});
  std::ostringstream user;
  user << context
       << "\n\nDescribe the surrounding art of this crop. Do not describe the marked "
          "gaps and do not say what should be drawn there. Reply with prose and exactly "
          "one fenced json block with materials, tones, light, density and edges.";
  messages.push_back({"user", user.str()});

  const ImageList images{base64};

  const bool accepted = ai->generateStream(
      contextId, messages,
      [this](const std::string &token) {
        std::lock_guard<std::mutex> lock(mutex);
        if (cancelled) return;
        if (!token.empty()) {
          reply += token;
          return;
        }
        streamDone = true;
        finished.notify_all();
      },
      std::stop_token{}, images);

  std::unique_lock<std::mutex> lock(mutex);
  if (!accepted) {
    busy = false;
    result = Style{};
    result.error = "the vision backend could not accept the request";
    result.raw = reply;
    hasResult = true;
    finished.notify_all();
    return;
  }

  // The empty-token sentinel is completion; generateStream returning means only that
  // the request was dispatched. Waiting here is the worker thread's job -- the frame
  // loop never blocks on this.
  finished.wait_for(lock, std::chrono::milliseconds(kTimeoutMs),
                    [this]() { return streamDone || cancelled; });

  if (cancelled) {
    busy = false;
    hasResult = false;
    return;
  }

  if (!streamDone) {
    result = Style{};
    result.error = "the vision model did not answer within the time budget";
    result.raw = reply;
  } else {
    const std::string complete = reply;
    lock.unlock();
    result = ParseReply(complete);
    lock.lock();
  }

  busy = false;
  hasResult = true;
  finished.notify_all();
}

}  // namespace StyleDescriber
