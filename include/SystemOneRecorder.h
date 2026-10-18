#pragma once

// System 1: the typed-classifier layer, and the record of every call made to it.
//
// System 1 is a *different model class* from the System 2 chat model. It does not
// generate prose: state and questions go in, calibrated answers come out. The
// interface below is the one `docs/agent-attention-design.md` §4.2 specifies, and
// nothing here assumes a vendor -- a hosted decision model, a local ONNX
// classifier and a rules fallback are all "System 1" if they implement it.
//
// Two things live in this file and they are deliberately separate:
//
//   * `SystemOneClassifier` -- the provider abstraction, mirroring `AI` and its
//     three backends.
//   * `SystemOneRecorder`   -- the running record of what was asked and answered.
//
// The recorder is separate from the classifier rather than a base class, so a
// provider can be used without recording it and the recorder can outlive any
// particular provider (see §"Wiring" below). This matters because the thing the
// overview window exists to decide -- is this model worth keeping -- is a
// question about *observations*, and observations must survive a backend swap.
//
// ---------------------------------------------------------------------------
// Wiring (three places, when the first call site lands)
// ---------------------------------------------------------------------------
//
//   1. A provider implements `SystemOneClassifier`. Call `recorder->Record(...)`
//      once per completed call -- success, refusal or timeout, and especially the
//      last two, which are the most informative samples the window will ever show.
//   2. The call site is given the recorder and the classifier. It passes a
//      scenario id and a short human description so a record can be read back
//      without the caller's source in front of you.
//   3. `Game` owns one recorder for the run (see `Game::systemOneRecorder`) and
//      the window reads it. There is no queue and no thread hop: `Record` is
//      already thread-safe, because System 1 is a network call and therefore runs
//      off the main thread (see `agent-attention-design.md` §5.1).
//
// Nothing in this file may write agent-chosen state. That is the advisory rule
// from `agent-attention-design.md` §4, and `SystemOneRecord::advisory` exists to
// make a violation visible: a record written with `advisory == false` is a call
// whose verdict changed behaviour, and the window flags those.

#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <unordered_map>
#include <vector>

namespace SystemOne {

// Defined below. Forward-declared because the provider interface attaches one,
// and the record type the recorder keeps is not needed to declare that.
class SystemOneRecorder;

// The kinds of question a System 1 model can answer. These are the four
// primitives the System One protocol defines, and they are distinct because each
// has a different *answer* shape, not merely a different wording:
//
//   Score       -- "does this condition hold?"      -> one probability, no confidence
//   Value       -- "where on this ordered scale?"   -> a level mean plus per-level probabilities
//   Choice      -- "which one of these options?"    -> the winning option plus per-option probabilities
//   Probability -- a bare yes/no likelihood, for a backend with no richer primitive
//
// The names follow the protocol rather than the design note where the two
// disagree: `agent-attention-design.md` §4.2 calls the first Noul, which is
// TypeSafe's spelling of the primitive. A backend that supports one need not
// support the others, so `Answer`'s defaults mean "no opinion" and a caller that
// reads a default as a real answer owns that bug.
enum class OptionType {
  // Yes/no. Maps to the protocol's `noul` primitive.
  Score,
  // Ordered scale, 2 to 10 levels. Maps to the protocol's `score` primitive.
  Value,
  // One of N labelled alternatives. Maps to the protocol's `choice` primitive.
  Choice,
  // A yes/no likelihood for backends that have only this much. Same answer shape
  // as Score; kept separate so a caller can tell a real Noul from a fallback.
  Probability,
};

struct Option {
  // Stable identifier for the question, unique within one request. It is what
  // the answer is keyed by, so a reply that comes back in another order, or
  // omits a question entirely, is still attributable.
  std::string id;
  OptionType type = OptionType::Score;

  // What to ask, in the imperative. Short: a System 1 model is not a reasoning
  // model, and a paragraph of context produces a worse calibrated answer than a
  // single sentence does.
  std::string instructions;

  // Choice only: the alternatives, keyed by the label the *engine* wants back.
  // The protocol matches on an arbitrary key, so a choice answer is a string the
  // caller chose rather than an index into a list -- which also survives the
  // model reordering its reasoning.
  std::vector<std::pair<std::string, std::string>> options;

  // Value only: the ordered levels, low to high. Two to ten of them, per the
  // protocol's limit.
  std::vector<std::string> levels;
};

// One answer. Defaults mean "no opinion" rather than zero, so a question the
// model did not answer and a question it answered "no" are distinguishable.
struct Answer {
  // Echoes Option::id. Present so a reply can be attributed even when the caller
  // asked several questions in one request.
  std::string id;

  // Score and Probability: probability that the condition holds, 0..1.
  float probability = 0.0f;

  // Value: the probability-weighted mean of the level indices, fractional, so it
  // sits between 0 and levels.size()-1. Normalise before weighting -- jumping
  // straight to 0..1 by dividing by the level count is the off-by-one the
  // protocol's own guidance warns about.
  float score = 0.0f;

  // Choice: the key of the winning option, as given in Option::options. Empty
  // means no choice was returned. The protocol has no "none of these" answer, so
  // a caller that needs one must ask it as an explicit option.
  std::string choice;

  // Probability that the model chose to *act* on its answer rather than defer,
  // 0..1, when the backend reports one. Laya's decision head produces it; hosted
  // Jev does not. Left at 0 when absent, which is why it is a separate field and
  // not folded into `confidence` -- "did not say" and "said zero" must not read
  // the same.
  float actionProbability = 0.0f;

  // How sure the model is of the above, 0..1. Absent (left at 0) on Score, which
  // the protocol defines without one. Separate from the answer itself: a
  // low-probability event can be a confident "no".
  float confidence = 0.0f;

  // Value only: the level descriptions, echoed back by the endpoint and keyed by
  // level index as a decimal string. Kept because a score is meaningless without
  // the scale it is on, and the reply's copy is what the score was actually
  // measured against -- re-deriving it from the request would hide a mismatch.
  std::unordered_map<std::string, std::string> legend;

  // The full distribution, keyed by option label for Choice and by level index
  // as a decimal string for Value. Kept because a threshold rule sometimes wants
  // the runner-up, and because the overview window shows it.
  std::unordered_map<std::string, float> probabilities;
};

// Everything a call produced, beyond the answers themselves: what was sent, what
// came back, and what it cost. Filled by the provider and handed to both the
// caller and the recorder in the same callback, so a record cannot describe a
// request the caller did not make.
struct CallTrace {
  // The exact JSON body posted. Empty for a backend that speaks another protocol
  // or never got as far as building one.
  std::string requestPayload;
  // The raw reply, before parsing. Empty on a transport failure.
  std::string responseText;

  // Empty on success. A transport failure, an HTTP error, a refused connection or
  // an unparseable body all land here -- a blank trace would hide the most
  // informative sample in the log.
  std::string error;

  // -1 means "not measured", following AIRequestStats' rule that a backend which
  // cannot measure must report nothing rather than a fabricated zero.
  int64_t latencyMs = -1;
  int promptTokens = -1;
  int outputTokens = -1;

  bool Ok() const { return error.empty(); }
};

// What a call site says about a request, for the record. Both fields are
// free-form because the set of call sites grows, but both are stable once a site
// ships: the window groups by `scenario` and the log is grepped by it.
struct Attribution {
  // Which job this was: "attention_gate", "site_preference", "task_validator",
  // "navigation".
  std::string scenario;
  // One line for the list: "Pietro, event: Margarethe nearby".
  std::string summary;
  // True when a harness rather than the game is asking. Keeps hand-written probes
  // out of the log file that the game's real traffic is judged from; see
  // SystemOneRecord::harness.
  bool harness = false;
  // False when the verdict changed behaviour rather than merely advising. The
  // default is the honest one for a call site that does not say: the gate and the
  // validator are advisory, and a new call site that forgets to say otherwise
  // shows up as advisory until its own record says not. Navigation is the first
  // non-advisory site -- its choice moves the character -- so it sets this false
  // rather than relying on the default.
  bool advisory = true;
};

// The provider interface. Mirrors `AI`: an async callback rather than a blocking
// return, because every backend is a network call and the main thread may not
// block on it.
class SystemOneClassifier {
public:
  using Callback = std::function<void(const std::vector<Answer> &, const CallTrace &)>;

  virtual ~SystemOneClassifier() = default;

  // `state` is the engine's own description of the situation -- facts assembled
  // from components, never chat history. `questions` are asked in ONE request:
  // System 1 models evaluate several questions in parallel and charge almost
  // nothing for the extra ones, so batching is the intended usage rather than an
  // optimisation.
  //
  // `onDone` may be invoked on another thread. It must be called exactly once,
  // including on failure, with one answer per question (a neutral answer is
  // acceptable; omitting one silently is not, because the caller cannot tell a
  // missing question from a "no"). Each answer carries its question's id.
  //
  // `stoken` cancels an in-flight request, so an agent interrupted mid-decision
  // does not hold a socket open. A cancelled call still reports through `onDone`
  // with `CallTrace::error` set, because a cancellation the caller cannot see is
  // indistinguishable from a backend that never answered.
  //
  // `attribution` is what makes a recorded call readable later. It is passed in
  // rather than derived because only the call site knows what the state means in
  // domain terms -- a provider sees a string, not "Pietro, deciding where to sow".
  virtual void Ask(const std::string &state,
                   const std::vector<Option> &questions, Callback onDone,
                   std::stop_token stoken = {}, Attribution attribution = {}) = 0;

  // Attaches the run's recorder, so every completed call is recorded without the
  // call site having to remember to. Null detaches.
  //
  // On the interface rather than left to each provider because the alternative --
  // every provider reimplementing the same bookkeeping, or every call site
  // duplicating it -- is how instrumentation gets skipped. A provider that does
  // not call it simply produces no records.
  virtual void SetRecorder(SystemOneRecorder *recorder) = 0;

  // False means "no backend is usable". Callers must degrade to their
  // non-System-1 path rather than waiting; see `agent-attention-design.md` §11.
  virtual bool Available() const = 0;

  // For the window's model column and for a record's provenance. A provider that
  // reports nothing here makes its own records unattributable.
  virtual std::string ModelLabel() const = 0;
};

// One completed call, as the overview window shows it and as the log file keeps
// it. Everything a keep-or-cut decision needs and nothing else.
struct SystemOneRecord {
  // When the call completed, "HH:MM:SS". Wall clock rather than a frame count,
  // because the point is to correlate with what the game did next.
  std::string clockTime;

  // Which job this was: "attention_gate", "site_preference", "task_validator".
  // Free-form because the set grows, but stable because the window groups by it
  // and the log is grepped by it.
  std::string scenario;

  // Human-readable one-liner for the list: "Pietro, event: Margarethe nearby".
  // Written by the call site, which is the only party that knows what the state
  // means in domain terms.
  std::string summary;

  // The provider's own model label, so records from different backends are
  // distinguishable after a swap.
  std::string model;

  // The exact text handed to the model, and the exact text back. Both are kept
  // verbatim: a truncated request cannot be hand-tuned against, and a rendered
  // summary of the reply hides the shapes that make a bad answer diagnosable.
  std::string requestPayload;
  std::string responseText;

  // The parsed answer, keyed by Option::id. Empty when parsing failed, which is
  // exactly the case `Error` then describes.
  std::unordered_map<std::string, Answer> answers;

  // "HH:MM:SS.mmm" on the record, plus measured fields. `latencyMs` is -1 when
  // the provider did not measure it, matching AIRequestStats' rule that a
  // backend which cannot measure reports nothing rather than a fabricated zero.
  int64_t latencyMs = -1;
  // -1 means "not reported". Both are summed across the whole request when a
  // backend charges per token.
  int promptTokens = -1;
  int outputTokens = -1;

  // Empty on success. A timeout, a refused connection, an HTTP error or an
  // unparseable reply all land here -- a blank row would hide the most
  // informative sample in the log. `ParsedOk()` is the shortcut.
  std::string error;

  // False when the verdict could have changed behaviour. Advisory-mode calls are
  // true; see the file header. The window counts and highlights the false ones.
  bool advisory = true;

  // False for a call made by a *harness* rather than by the game -- the
  // `--systemone-test` probe, or a canned turn in a self-test mode.
  //
  // Recorded and shown like any other call, because a deliberately fixed question
  // is the best way to compare two backends. But never written to the log file:
  // that file is evidence about how the model behaves on the game's real traffic,
  // and mixing hand-written probes into it would make every count in it wrong.
  bool harness = false;

  bool ParsedOk() const { return error.empty() && !answers.empty(); }
};

// A non-call event worth keeping beside the calls: a profile switch, a probe
// failure, a backend going away. Kept separate from SystemOneRecord because it is not
// an observation of a model and must not inflate the call counts the window shows.
struct SystemOneNote {
  std::string clockTime;
  std::string text;
};

// The running record: a bounded ring in memory, plus an append-only file.
//
// Thread-safe by construction. `Record` is called from the provider's worker
// thread and `Snapshot` from the draw loop, so the mutex is not optional and a
// shared_ptr copy-out avoids holding it while ImGui draws.
class SystemOneRecorder {
public:
  // `maxEntries` bounds memory only; the file keeps everything. A long session
  // can produce thousands of these, and the window only ever shows the newest.
  explicit SystemOneRecorder(std::size_t maxEntries = 500);

  // Appends the record and writes it out. Never throws and never fails the
  // caller: a session with nowhere to write still runs, it is just not recorded,
  // which is reported once through the warning sink.
  void Record(SystemOneRecord record);

  // Newest first, so the window can draw in order without reversing per frame.
  std::vector<SystemOneRecord> Snapshot() const;

  std::size_t Count() const;
  void Clear();

  // Appends an event to the log file and keeps it for the window, without creating a
  // call record. Used for things that explain a run rather than describe a decision --
  // above all "the backend was switched here", which is otherwise only inferable from
  // the model label changing between two records.
  void Note(const std::string &text);

  // Newest first, bounded, for the window's event strip.
  std::vector<SystemOneNote> RecentNotes() const;

  // How many records used a given scenario id, and how many were not advisory.
  // Cheap enough for per-frame use at the sizes involved.
  std::unordered_map<std::string, int> ScenarioCounts() const;
  int NonAdvisoryCount() const;

  // Where records are being written, or empty when no file could be opened. The
  // window shows this so a failed log is visible rather than assumed.
  std::string LogFilePath() const;

  // Called once if the log file cannot be created. The game's DebugLog is the
  // intended sink; keeping it a callback means this file does not need Game.
  void SetWarningSink(std::function<void(const std::string &)> sink);

private:
  // Lazily created on the first record, not in the constructor: a run that never
  // calls System 1 should not leave an empty log file behind, and the designer
  // window's log uses the same rule.
  void EnsureLogFile();

  mutable std::mutex mutex;
  std::deque<SystemOneRecord> entries;
  std::size_t maxEntries;

  // Empty until the first successful open; a single space marks a failed attempt
  // so later records re-try instead of spinning on an empty path.
  std::string logFilePath;
  bool warnedAboutLog = false;
  std::function<void(const std::string &)> warningSink;

  int nonAdvisoryCount = 0;
  std::unordered_map<std::string, int> scenarioCounts;

  // Bounded: notes are context for the calls around them, and a long session would
  // otherwise accumulate one per switch forever.
  static constexpr std::size_t kMaxNotes = 64;
  std::deque<SystemOneNote> notes;
};

}  // namespace SystemOne
