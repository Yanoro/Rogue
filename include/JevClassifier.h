#pragma once

#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#include "SystemOneRecorder.h"

namespace SystemOne {

// The concrete System 1 provider: a decision model that speaks the System One
// protocol, reached over HTTP.
//
// It is named after Jev because that is the model the protocol was written
// against, but nothing in the request or the parsing is Jev-specific. The
// protocol is what is implemented -- `state` plus a map of typed questions in,
// typed answers out -- and 21 decision models from 13 makers use the same
// endpoint, so pointing this at another one is a change of `model`, not of code.
// When a better model arrives, it is a config edit.
//
// What it is not: a chat model. It does not generate prose, it cannot be asked an
// open question, and it returns no reasoning. `Ask` therefore takes typed
// questions and returns typed answers, which is why this class is small.
//
// Protocol reference: the System One / Decisions API as documented at
// https://openrouter.ai/docs/guides/community/jev
//
//   POST <endpoint>
//   Authorization: Bearer <apiKey>
//   { "model": "...", "state": <string|array|object>,
//     "questions": { "<id>": { "type": "choice"|"noul"|"score",
//                              "instructions": "...", "criteria": ... } } }
//
// Answers come back keyed by the same question id, each with its own shape:
//   choice -> { "choice": "<key>", "probabilities": {...}, "confidence": f }
//   noul   -> { "noul": f }
//   score  -> { "score": f, "probabilities": {...}, "confidence": f }
//
// Two consequences of that shape are worth stating because they are easy to get
// wrong from the outside:
//
//   * A `choice` answer is the option's *key*, an arbitrary string. It is not an
//     index, and the protocol deliberately lets the caller choose the key.
//   * `noul` has no confidence field. A missing confidence is written as 0, not
//     defaulted to 1 -- "the model did not say" must not read as "certain".
class JevClassifier : public SystemOneClassifier {
public:
  struct Config {
    // Full URL of the decisions endpoint. Defaulted to OpenRouter's, which is the
    // documented way to reach Jev with an ordinary API key and no separate
    // TypeSafe account.
    std::string endpoint = "https://openrouter.ai/api/alpha/decisions";
    // Bearer token. Empty makes Available() false rather than letting every call
    // fail one by one.
    std::string apiKey;
    // The model id. The `~` alias tracks the newest release, which is the right
    // default while thresholds are still being tuned; pin a version once they are.
    std::string model = "~typesafe/jev-latest";
    // Whole-request budget. A decision model answers in roughly 100ms, so a
    // request still running after this is not going to produce a useful verdict --
    // and an agent's turn must not block on a hung socket.
    long timeoutSeconds = 30;

    // True for a server on this machine. Used to default the timeout, because the
    // hosted endpoint answers in ~100ms while a 4B model on a CPU takes seconds --
    // a local server behind the hosted 30s budget times out on a perfectly healthy
    // request. Matched on the URL rather than configured, so pointing at a local
    // model does not require also remembering to raise the timeout.
    bool IsLocalEndpoint() const {
      return endpoint.find("127.0.0.1") != std::string::npos ||
             endpoint.find("localhost") != std::string::npos ||
             endpoint.find("[::1]") != std::string::npos;
    }
  };

  // The concrete backends this provider has been run against, as presets rather
  // than as magic strings scattered through config and harness code.
  //
  // Why presets and not a subclass each. Kev speaks the *same* `/v1/systemone`
  // protocol as Jev, so a `KevClassifier : JevClassifier` would differ only in two
  // default strings -- duplicate code whose only effect is that a parser fix has to
  // be made twice. The thing that actually differs between backends is the
  // configuration, so that is what is named here.
  //
  // Why these two and not more: they are the ones this codebase has genuinely
  // exercised. The other open decision models (Lev, Laya, Julia-1, OpenJev) take the
  // identical request and answer shape, so they need no preset -- point `endpoint`
  // and `model` at a `llama-server` running them and everything here applies
  // unchanged. Adding a preset for a model nobody has run would be a guess
  // presented as support.
  // How long a decision from a local server is allowed to take. Generous on
  // purpose: a 4B model on a CPU is ~10s for three short questions and several
  // times that for a long state, and a timeout that fires on healthy work is worse
  // than waiting.
  static constexpr long kLocalTimeoutSeconds = 120;

  struct Preset {
    // Hosted Jev through OpenRouter. The default, and the only one that needs a
    // real key: an OpenRouter key reaches Jev with no separate TypeSafe account.
    static Config HostedJev() { return Config{}; }

    // Kev-4B on a `llama-server` on this machine.
    //
    // `apiKey` is a placeholder, not a credential: llama-server binds 127.0.0.1,
    // requires no authentication, and ignores the header. It has to be non-empty
    // anyway because `Available()` treats an empty key as "not configured" and
    // would refuse before sending anything -- so "no key needed" is spelled as a
    // key that is obviously not one, rather than left blank.
    //
    // `model` is likewise advisory: llama-server reports the path it loaded and
    // ignores the requested id. It is still set, because it is what the log and the
    // overview window show when hunting for which backend answered.
    static Config LocalKev() {
      Config config;
      config.endpoint = "http://127.0.0.1:9931/v1/systemone";
      config.model = "local";
      config.apiKey = "local-no-key-needed";
      // A local model answers in seconds, not the ~100ms of the hosted endpoint,
      // and a 4B model on a CPU is slower still (~10s measured). The hosted default
      // would cut off a slow but perfectly good local answer.
      config.timeoutSeconds = kLocalTimeoutSeconds;
      return config;
    }
  };

  explicit JevClassifier(Config config);
  // Waits for any request still in flight before returning, so a provider can be
  // replaced at runtime (a model-switch window) without racing its own worker. See
  // the in-flight members below for why that wait exists at all.
  ~JevClassifier() override;

  // Builds the request, posts it on a worker thread, and reports through the
  // callback. Never blocks: a decision that takes 100ms is still a network call,
  // and this may be called from the main thread.
  //
  // The callback always fires exactly once, including on a transport failure, an
  // HTTP error, an unparseable body and cancellation. `questions` is not held by
  // reference past this call.
  void Ask(const std::string &state, const std::vector<Option> &questions,
           Callback onDone, std::stop_token stoken = {},
           Attribution attribution = {}) override;

  void SetRecorder(SystemOneRecorder *recorder) override;

  bool Available() const override { return !config.apiKey.empty(); }

  std::string ModelLabel() const override { return config.model; }

  // The endpoint in use, for the settings UI and the log header.
  const std::string &Endpoint() const { return config.endpoint; }

  // Can the endpoint be reached at all? Used before accepting a profile switch.
  //
  // Deliberately a *transport* check, not a decision: it opens a TCP (and, for
  // https, TLS) connection and reports whether that succeeded. It sends no request,
  // spends no tokens, and knows nothing about model ids or API keys.
  //
  // That depth is what reconciles "refuse a broken switch" with "a cold local server
  // must still be selectable": a llama-server that is still loading the model answers
  // the connect and only then returns 503, so it counts as reachable, while a server
  // that is not running refuses the connect immediately. Deeper checks (a real
  // request) would cost money, and would make a healthy two-minute model load look
  // like a failure.
  //
  // `error` receives a short reason on failure, phrased for a status line.
  static bool ProbeReachable(const Config &config, std::string *error);

private:
  Config config;

  // Borrowed; outlives this object because Game owns the recorder for the whole
  // run. Guarded because SetRecorder may be called while a request is in flight.
  SystemOneRecorder *recorder = nullptr;
  mutable std::mutex recorderMutex;

  // In-flight bookkeeping, so destruction cannot race a request.
  //
  // `Ask` runs its work on a detached thread that captures `this`, which is only
  // sound while the provider outlives its own requests. That held while the provider
  // was built once per run for the whole run, and stopped holding the moment a
  // runtime UI can swap providers -- a model-switch window would destroy the old
  // classifier with a request still in the air, and the worker would go on touching
  // freed memory. It is a race, so the failure would be rare and read as a random
  // crash.
  //
  // The destructor therefore asks the workers to stop and waits for them, which is
  // cheaper and more predictable than trying to make `Ask` hold a shared_ptr to its
  // own owner. It is bounded: a worker checks `shuttingDown` between curl polls, so
  // it leaves within ~10ms; the timeout exists only so a wedged socket cannot hang
  // the shutdown of the whole game.
  std::mutex inflightMutex;
  std::condition_variable inflightChanged;
  int inflight = 0;
  bool shuttingDown = false;

  // curl_global_init is process-wide, so it is done once rather than per request.
  // std::once_flag rather than a static bool, because two providers constructed
  // concurrently must not both initialise it.
  static void EnsureCurlInitialised();
};

}  // namespace SystemOne
