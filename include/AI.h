#pragma once
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <stop_token>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct ChatMessage {
  std::string role;
  std::string content;
};

// A request may carry images alongside the text. They are base64-encoded images
// (PNG in practice, no data: prefix), which is what the Ollama JSON API expects
// verbatim.
//
// Images are deliberately *not* a ChatMessage field: they are per-request input,
// not conversation state. Putting a multi-megabyte base64 blob in `history` would
// make every NPC context, log line and transcript copy it forever.
using ImageList = std::vector<std::string>;

// Where one request's time actually went, when the backend measures it.
//
// This exists because wall-clock time alone cannot answer "why was that slow?":
// a cold model, a huge image and a rambling answer all look the same from the
// outside. Ollama reports the breakdown in its /api/generate response; a backend
// that does not leaves `valid` false and the caller shows nothing rather than
// inventing numbers.
struct AIRequestStats {
  bool valid = false;
  // Tokens the model had to read: the prompt text plus every image token. For a
  // screenshot this is the expensive half of "reading".
  int promptTokens = 0;
  // Tokens it generated.
  int outputTokens = 0;
  // The API reports nanoseconds; seconds here. Load is paid only when the model
  // was cold, which is why it dominates an occasional request and nothing else.
  double loadSeconds = 0.0;
  double promptEvalSeconds = 0.0;
  double evalSeconds = 0.0;
  double totalSeconds = 0.0;
};

class AI {
public:
  using StreamCallback = std::function<void(const std::string &)>;
  std::function<void(const std::string&)> errorLogger;
  void setErrorLogger(std::function<void(const std::string&)> logger) {
    errorLogger = logger;
  }

  // `images` is optional and defaults to empty, so a text-only caller is
  // unchanged and a provider that does not support vision may ignore it.
  virtual std::string generate(const std::string &contextId,
                               const std::vector<ChatMessage> &history,
                               std::stop_token stoken = {},
                               const ImageList &images = {}) = 0;
  virtual bool generateStream(const std::string &contextId,
                              const std::vector<ChatMessage> &history,
                              StreamCallback callback,
                              std::stop_token = {},
                              const ImageList &images = {}) = 0;
  virtual bool isBusy(const std::string &contextId) = 0;
  virtual std::string getLastMessage(const std::string &contextId) = 0;
  virtual std::string getContext(const std::string &contextId) = 0;

  // Forgets everything the provider has associated with `contextId`, so the next
  // request on it starts from nothing. Providers that keep a server-side
  // conversation (Ollama's token context) need this to make one request
  // independent of the last; stateless providers inherit the no-op default, and
  // the caller usually owns the history anyway.
  //
  // Must not be called while a request on `contextId` is in flight.
  virtual void resetContext(const std::string & /*contextId*/) {}

  // Fills `out` with the breakdown of the most recent completed request on
  // `contextId`. Returns false, and leaves `out` alone, when the backend does not
  // measure one. Callers treat false as "no breakdown available", not as an
  // error.
  virtual bool getLastStats(const std::string & /*contextId*/,
                            AIRequestStats & /*out*/) {
    return false;
  }

  // Turns the backend's chain-of-thought on or off, where the backend has one.
  //
  // Exists because reasoning is the wrong tool for some requests and actively harmful
  // for short ones: a backend that spends its completion budget thinking returns *no
  // answer at all*, which a caller sees as an empty reply rather than as an error.
  // Measured on OpenRouter's deepseek-v4-flash, which enables reasoning by default --
  // a rewrite request came back with `content: None` and tokens still billed.
  //
  // The default does nothing, so a backend with no reasoning mode is unchanged and a
  // caller can ask unconditionally.
  virtual void setReasoningEnabled(bool /*enabled*/) {}

  virtual std::string getAIName() const = 0;
  virtual std::string getModelName() const = 0;
  virtual std::string getProviderName() const { return "Unknown"; }
  virtual std::string getQuantization() const { return "Unknown"; }
  virtual std::string getAdditionalInfo() const { return ""; }
  virtual ~AI() = default;
};
