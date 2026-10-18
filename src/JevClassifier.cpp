#include "JevClassifier.h"

#include <curl/curl.h>

#include <chrono>
#include <iostream>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>

#include "TimeUtils.hpp"

namespace SystemOne {
namespace {

std::once_flag curlInitFlag;

std::size_t WriteCallback(void *contents, std::size_t size, std::size_t nmemb,
                          void *userp) {
  static_cast<std::string *>(userp)->append(static_cast<char *>(contents),
                                            size * nmemb);
  return size * nmemb;
}

// The primitive name the protocol uses for each of our question kinds.
//
// Score and Probability both map to `noul`: one is a real yes/no judged by a
// model that has the primitive, the other is the same answer requested from a
// backend that has nothing richer. They stay distinct in `OptionType` so a caller
// can tell the two apart, but on the wire they are one question type.
const char *PrimitiveFor(OptionType type) {
  switch (type) {
    case OptionType::Choice:
      return "choice";
    case OptionType::Value:
      return "score";
    case OptionType::Score:
    case OptionType::Probability:
    default:
      return "noul";
  }
}

// Every answer, one per question, so a caller can index by question rather than
// hunting a sparse reply. A question the model did not answer comes back neutral,
// which is what lets a caller distinguish "no" from "no opinion".
std::vector<Answer> NeutralAnswers(const std::vector<Option> &questions) {
  std::vector<Answer> answers;
  answers.reserve(questions.size());
  for (const Option &question : questions) {
    Answer answer;
    answer.id = question.id;
    answers.push_back(std::move(answer));
  }
  return answers;
}

// Registers a worker as in flight for its whole life, so the destructor can wait for
// it. Declared inside the worker and destroyed at every exit path, including the
// early returns that skip the request entirely -- a manual decrement would leak one
// count per early return and make the destructor wait for its full timeout.
struct InflightGuard {
  std::mutex &mutex;
  std::condition_variable &changed;
  int &count;

  // Decrement-only by design. The corresponding increment happens in Ask *before*
  // the thread is created: if the count were raised inside the worker instead, the
  // destructor could check "nothing in flight", see zero, and return while a thread
  // was already committed to running.
  InflightGuard(std::mutex &m, std::condition_variable &c, int &n)
      : mutex(m), changed(c), count(n) {}

  ~InflightGuard() {
    // The notify happens *while holding the lock*, and that is load-bearing rather
    // than style. A waiter must reacquire the mutex to observe the change, so it
    // cannot proceed until this thread releases -- which happens after the count is
    // decremented and after the notify returns. Notifying after unlocking would let
    // the destructor finish and free these members while the notify was still
    // running on them.
    std::lock_guard<std::mutex> lock(mutex);
    --count;
    changed.notify_all();
  }

  InflightGuard(const InflightGuard &) = delete;
  InflightGuard &operator=(const InflightGuard &) = delete;
};

void ApplyChoice(const nlohmann::json &body, Answer &answer) {
  if (body.contains("choice") && body["choice"].is_string()) {
    answer.choice = body["choice"].get<std::string>();
  }
  if (body.contains("probabilities") && body["probabilities"].is_object()) {
    for (const auto &entry : body["probabilities"].items()) {
      if (entry.value().is_number()) {
        answer.probabilities[entry.key()] =
            entry.value().get<float>();
      }
    }
  }
  if (body.contains("confidence") && body["confidence"].is_number()) {
    answer.confidence = body["confidence"].get<float>();
  }
}

void ApplyOrdered(const nlohmann::json &body, Answer &answer) {
  // `noul` and `score` carry their headline value under their own name, so which
  // field to read is decided by what is present rather than by which primitive we
  // asked for. That keeps the parser honest when a backend answers a question with
  // a different primitive than the one requested.
  if (body.contains("noul") && body["noul"].is_number()) {
    answer.probability = body["noul"].get<float>();
  }
  if (body.contains("score") && body["score"].is_number()) {
    answer.score = body["score"].get<float>();
  }
  if (body.contains("probabilities") && body["probabilities"].is_object()) {
    for (const auto &entry : body["probabilities"].items()) {
      if (entry.value().is_number()) {
        answer.probabilities[entry.key()] = entry.value().get<float>();
      }
    }
  }
  if (body.contains("legend") && body["legend"].is_object()) {
    for (const auto &entry : body["legend"].items()) {
      if (entry.value().is_string()) {
        answer.legend[entry.key()] = entry.value().get<std::string>();
      }
    }
  }
  if (body.contains("confidence") && body["confidence"].is_number()) {
    answer.confidence = body["confidence"].get<float>();
  }
  // Laya's decision head reports the probability of *acting* on its answer, which
  // hosted Jev does not. Kept separate from `confidence` because they mean
  // different things.
  if (body.contains("rl_agent") && body["rl_agent"].is_number()) {
    answer.actionProbability = body["rl_agent"].get<float>();
  }
}

// The object holding per-question answers, whichever envelope the server used.
//
// Two shapes are in the wild for the same protocol, which is why both have to
// work before a local model can replace the hosted one without a code change:
//
//   hosted (OpenRouter decisions)  ->  { "answers": { "<id>": {...} }, "usage": ... }
//   llama.cpp / localai / vllm.cpp ->  { "choices": [ { "message": { "content":
//                                          { "<id>": {...} } } ], "usage": ... }
//
// Returns null when neither is present, which the caller reports rather than
// quietly returning a neutral answer for every question.
const nlohmann::json *ExtractAnswers(const nlohmann::json &root) {
  if (root.contains("answers") && root["answers"].is_object()) {
    return &root["answers"];
  }
  if (root.contains("choices") && root["choices"].is_array() &&
      !root["choices"].empty()) {
    const nlohmann::json &message = root["choices"][0]["message"];
    if (message.is_object() && message.contains("content")) {
      const nlohmann::json &content = message["content"];
      // The content is the answers object itself on the local servers. A string
      // carrying serialised JSON is also accepted, because a chat-shaped proxy in
      // front of a decision model will wrap it that way.
      if (content.is_object()) return &content;
      if (content.is_string()) {
        static thread_local nlohmann::json parsed;
        parsed = nlohmann::json::parse(content.get<std::string>(), nullptr, false);
        if (parsed.is_object()) return &parsed;
      }
    }
  }
  return nullptr;
}

// The usage counters, under either spelling. The decisions API uses
// `input_tokens`/`output_tokens`; the local servers use the chat spelling.
void ApplyUsage(const nlohmann::json &root, CallTrace &trace) {
  if (!root.contains("usage") || !root["usage"].is_object()) return;
  const nlohmann::json &usage = root["usage"];
  for (const char *key : {"input_tokens", "prompt_tokens"}) {
    if (usage.contains(key) && usage[key].is_number_integer()) {
      trace.promptTokens = usage[key].get<int>();
      break;
    }
  }
  for (const char *key : {"output_tokens", "completion_tokens"}) {
    if (usage.contains(key) && usage[key].is_number_integer()) {
      trace.outputTokens = usage[key].get<int>();
      break;
    }
  }
}

// Parses the reply into `answers` in the order the questions were asked. Returns
// an empty string on success, or a message naming what went wrong.
//
// The envelope is the decisions API's own, not an OpenAI chat completion:
//
//   { "answers": { "<id>": { "type": ..., ... } }, "usage": {...}, "model": ... }
//
// Verified against a live `typesafe/jev-1.14` reply rather than inferred, because
// the two shapes are easy to confuse and a wrong guess here fails as "no answers"
// rather than as a parse error. A reply with no `answers` object is reported as
// such instead of silently yielding neutral answers for every question.
//
// Ordered by the *questions* rather than by the reply, so a model that returns its
// answers in another order -- or omits one -- cannot shift every answer onto the
// wrong question.
std::string ParseAnswers(const nlohmann::json &root,
                         const std::vector<Option> &questions,
                         std::vector<Answer> &answers) {
  if (!root.is_object()) {
    return "reply was not a JSON object";
  }
  if (root.contains("error")) {
    // The protocol reports failures in-band as an object, sometimes with an HTTP
    // 200 around it. Surfaced verbatim because the message is the diagnosis.
    const nlohmann::json &error = root["error"];
    if (error.is_object() && error.contains("message")) {
      return "API error: " + error["message"].dump();
    }
    return "API error: " + error.dump();
  }

  const nlohmann::json *answersPtr = ExtractAnswers(root);
  if (answersPtr == nullptr) {
    return "reply carried no answers (neither an 'answers' object nor a choices "
           "envelope)";
  }
  const nlohmann::json &answersJson = *answersPtr;

  for (std::size_t i = 0; i < questions.size(); ++i) {
    const auto found = answersJson.find(questions[i].id);
    if (found == answersJson.end() || !found->is_object()) {
      // Left neutral. Not an error on its own: a question the model skipped is a
      // real outcome, and the caller decides whether it matters.
      continue;
    }
    if (questions[i].type == OptionType::Choice) {
      ApplyChoice(*found, answers[i]);
    } else {
      ApplyOrdered(*found, answers[i]);
    }
    // Per-answer `reasons` exist in the protocol but are deliberately not carried:
    // Answer is the typed contract and must stay free of prose, and the whole raw
    // reply is already in CallTrace::responseText for anyone who wants to read
    // them. Dropping them here is a decision, not an oversight.
  }
  return {};
}

}  // namespace

void JevClassifier::EnsureCurlInitialised() {
  std::call_once(curlInitFlag, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

JevClassifier::JevClassifier(Config config) : config(std::move(config)) {
  EnsureCurlInitialised();
}

JevClassifier::~JevClassifier() {
  {
    std::lock_guard<std::mutex> lock(inflightMutex);
    shuttingDown = true;
  }
  // Wake any worker sitting in curl_multi_wait so it notices `shuttingDown` instead
  // of waiting out its poll interval.
  inflightChanged.notify_all();

  std::unique_lock<std::mutex> lock(inflightMutex);
  if (inflight == 0) return;

  // Bounded, so a wedged socket cannot hang the shutdown of the game. The worker
  // checks the flag every poll (10ms), so in practice this returns almost at once;
  // the timeout only covers a request stuck inside curl itself, where continuing is
  // strictly better than refusing to close the game.
  const bool drained = inflightChanged.wait_for(
      lock, std::chrono::seconds(5), [this] { return inflight == 0; });
  if (!drained) {
    std::cerr << "System 1: shutting down with " << inflight
              << " request(s) still in flight; a wedged socket is the likely cause."
              << std::endl;
  }
}

bool JevClassifier::ProbeReachable(const Config &config, std::string *error) {
  if (config.endpoint.empty()) {
    if (error) *error = "no endpoint configured";
    return false;
  }
  EnsureCurlInitialised();

  CURL *curl = curl_easy_init();
  if (curl == nullptr) {
    if (error) *error = "could not initialise curl";
    return false;
  }

  // CONNECT_ONLY stops curl after the transport is up, so no HTTP request is sent and
  // no authorization is claimed. For https this still performs the TLS handshake,
  // which is what makes "unreachable" mean the same thing for a remote and a local
  // endpoint.
  curl_easy_setopt(curl, CURLOPT_URL, config.endpoint.c_str());
  curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 1L);
  // Short: this runs from a UI action, and a switch that hangs is worse than a switch
  // that reports "unreachable".
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 2000L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 4000L);

  const CURLcode result = curl_easy_perform(curl);
  curl_easy_cleanup(curl);

  if (result != CURLE_OK) {
    if (error) {
      *error = std::string("cannot reach ") + config.endpoint + " (" +
               curl_easy_strerror(result) + ")";
    }
    return false;
  }
  return true;
}

void JevClassifier::SetRecorder(SystemOneRecorder *newRecorder) {
  std::lock_guard<std::mutex> lock(recorderMutex);
  recorder = newRecorder;
}

void JevClassifier::Ask(const std::string &state,
                        const std::vector<Option> &questions, Callback onDone,
                        std::stop_token stoken, Attribution attribution) {
  // The request body, built here rather than in the worker so a caller can see
  // exactly what will be sent before it goes, and so the payload in the record is
  // the payload that was posted.
  nlohmann::json payload;
  payload["model"] = config.model;
  // The protocol accepts a string for `state`, and a string is what the interface
  // takes. Object states, which the protocol also allows for pointing questions at
  // fields, are a later addition: they need the question `instructions` to carry
  // backticked paths, which is a call-site concern rather than a transport one.
  payload["state"] = state;

  nlohmann::json questionMap = nlohmann::json::object();
  for (const Option &question : questions) {
    nlohmann::json entry;
    entry["type"] = PrimitiveFor(question.type);
    entry["instructions"] = question.instructions;
    switch (question.type) {
      case OptionType::Choice: {
        // criteria is an object keyed by the label the engine wants back. The key
        // is the answer, so it is the caller's vocabulary and not the model's.
        nlohmann::json criteria = nlohmann::json::object();
        for (const auto &option : question.options) {
          criteria[option.first] = option.second;
        }
        entry["criteria"] = criteria;
        break;
      }
      case OptionType::Value: {
        // The protocol takes an ordered array of level descriptions, low to high,
        // two to ten. Sent as written: normalising the returned score by
        // levels.size()-1 is the caller's job, because only the caller knows what
        // the scale means.
        entry["criteria"] = question.levels;
        break;
      }
      case OptionType::Score:
      case OptionType::Probability:
      default:
        // `noul` takes no criteria.
        break;
    }
    questionMap[question.id] = std::move(entry);
  }
  payload["questions"] = std::move(questionMap);

  const std::string payloadText = payload.dump(2);
  const std::string endpoint = config.endpoint;
  const std::string apiKey = config.apiKey;
  const long timeoutSeconds = config.timeoutSeconds;

  // A worker per request, detached, matching OpenRouterAI's model. A decision
  // model answers in ~100ms, so a thread pool would be more machinery than the
  // traffic justifies. The corollary is the same as that provider's: a classifier
  // destroyed while a call is in flight lets the worker touch freed memory, so
  // the owner must outlive its requests. Game owns this for the whole run.
  {
    // Counted here rather than inside the thread, because the destructor's "nothing
    // in flight" check must not be able to pass in the window between this thread
    // being created and it starting to run.
    std::lock_guard<std::mutex> lock(inflightMutex);
    if (shuttingDown) {
      // Being torn down: do not start work whose owner is going away. Reported
      // through the callback like any other failure, so a caller is never left
      // waiting for an answer that will not come.
      std::vector<Answer> answers = NeutralAnswers(questions);
      CallTrace trace;
      trace.requestPayload = payloadText;
      trace.error = "provider is shutting down";
      if (onDone) onDone(answers, trace);
      return;
    }
    ++inflight;
  }

  std::thread([this, questions, payloadText, endpoint, apiKey, timeoutSeconds,
               onDone, stoken, attribution]() mutable {
    // Decrements the count Ask already raised, on every exit path including the
    // early returns below.
    const InflightGuard inflightGuard(inflightMutex, inflightChanged, inflight);
    CallTrace trace;
    trace.requestPayload = payloadText;

    std::vector<Answer> answers = NeutralAnswers(questions);

    if (apiKey.empty()) {
      trace.error = "no API key configured";
      if (onDone) onDone(answers, trace);
      return;
    }

    const auto started = std::chrono::steady_clock::now();
    std::string readBuffer;
    long httpCode = 0;
    bool cancelled = false;
    bool timedOut = false;

    CURL *curl = curl_easy_init();
    if (curl == nullptr) {
      trace.error = "could not initialise curl";
      if (onDone) onDone(answers, trace);
      return;
    }

    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers,
                                ("Authorization: Bearer " + apiKey).c_str());
    // A decision endpoint bills per input token, so a timeout well under the
    // caller's patience is better than a socket held open for a verdict nobody
    // will read.
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSeconds);

    curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payloadText.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                     static_cast<long>(payloadText.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);

    CURLM *multi = curl_multi_init();
    curl_multi_add_handle(multi, curl);

    const auto deadline =
        started + std::chrono::seconds(timeoutSeconds + 2);
    int stillRunning = 1;
    while (stillRunning != 0) {
      curl_multi_perform(multi, &stillRunning);
      if (stillRunning == 0) break;

      if (stoken.stop_requested()) {
        cancelled = true;
        break;
      }
      // The owner is being destroyed. Leave promptly so its bounded wait is short;
      // the same path as a cancellation, because from the caller's point of view
      // both mean "no verdict".
      {
        bool stopping = false;
        {
          std::lock_guard<std::mutex> lock(inflightMutex);
          stopping = shuttingDown;
        }
        if (stopping) {
          cancelled = true;
          break;
        }
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        timedOut = true;
        break;
      }
      curl_multi_wait(multi, nullptr, 0, 10, nullptr);
    }

    CURLcode result = CURLE_OK;
    if (!cancelled && !timedOut) {
      int messagesLeft = 0;
      CURLMsg *message = nullptr;
      while ((message = curl_multi_info_read(multi, &messagesLeft)) != nullptr) {
        if (message->msg == CURLMSG_DONE) {
          result = message->data.result;
        }
      }
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    }

    curl_multi_remove_handle(multi, curl);
    curl_multi_cleanup(multi);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    trace.latencyMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - started)
                          .count();

    if (cancelled) {
      trace.error = "cancelled before an answer arrived";
    } else if (timedOut) {
      trace.error = "timed out after " + std::to_string(timeoutSeconds) + "s";
    } else if (result != CURLE_OK) {
      trace.error = std::string("transport error: ") + curl_easy_strerror(result);
    } else if (httpCode < 200 || httpCode >= 300) {
      // The body usually carries a JSON error object with the real reason, so it
      // is kept in the trace and quoted where it can be read.
      trace.error = "HTTP " + std::to_string(httpCode);
      if (!readBuffer.empty()) {
        trace.error += ": " + readBuffer.substr(0, 400);
      }
    } else {
      trace.responseText = readBuffer;
      const nlohmann::json root =
          nlohmann::json::parse(readBuffer, nullptr, false);
      if (root.is_discarded()) {
        trace.error = "reply was not valid JSON";
      } else {
        trace.error = ParseAnswers(root, questions, answers);
        ApplyUsage(root, trace);
      }
    }

    // Recorded whatever happened, before the caller is told. A timeout or an
    // unparseable reply is the most informative sample the overview window can
    // show, so nothing is filtered out here; the window's filter is the place to
    // hide noise, not the recorder.
    {
      SystemOneRecorder *target = nullptr;
      {
        std::lock_guard<std::mutex> lock(recorderMutex);
        target = recorder;
      }
      if (target != nullptr) {
        SystemOneRecord record;
        record.clockTime = TimeUtils::GetClockTimeString();
        record.scenario = attribution.scenario;
        record.summary = attribution.summary;
        record.model = config.model;
        record.requestPayload = trace.requestPayload;
        record.responseText = trace.responseText;
        record.latencyMs = trace.latencyMs;
        record.promptTokens = trace.promptTokens;
        record.outputTokens = trace.outputTokens;
        record.error = trace.error;
        record.advisory = attribution.advisory;
        record.harness = attribution.harness;
        for (const Answer &answer : answers) {
          record.answers[answer.id] = answer;
        }
        target->Record(std::move(record));
      }
    }

    if (onDone) onDone(answers, trace);
  }).detach();
}

}  // namespace SystemOne
