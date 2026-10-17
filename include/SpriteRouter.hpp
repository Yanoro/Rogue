#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <stop_token>

#include "JevClassifier.h"
#include "SpriteKinds.hpp"

// The one-shot router: which kind of sprite a request is for, whether it is one sprite or
// a set, and how many.
//
// Why a classifier rather than a heuristic. The request arrives as prose from a person --
// "a prison's furniture and fittings" -- and the choice it settles (tile or object, one or
// many) decides which instruction document the designer is built from. A keyword match
// would be wrong in exactly the cases that matter: "a crate that stores things" contains no
// tile-or-object word at all, and "some prison floors" is a plural set of tiles. Asking a
// decision model is what the System 1 layer exists for (see docs/agent-attention-design.md
// and docs/sprite-designer-design.md §3).
//
// Three questions in **one** request, because the protocol takes a map of questions and
// returns a map of answers: the kind, the shape, and the count. Asking them separately
// would be three round trips for one decision.
//
// What this class deliberately is NOT: a window, a cache, or a retry policy. It answers
// once, through a callback that always fires exactly once, and the caller decides what a
// low-confidence or unanswered verdict means. That is what keeps it testable with
// `--sprite-route` and no display.
namespace SpriteRouter {

// The set-size buckets, and what each means as a number.
//
// This is `Choice`, not `Value`, and the change was forced by measurement rather than
// taste. The design asked for a numeric `Value` question ("how many sprites?") with a
// documented fallback to buckets "if logged answers turn out to be nonsense"
// (docs/sprite-designer-design.md §3). They did, on the first five real requests:
//
//   "Create a rotting wooden floor tile"        -> count 1   (right)
//   "a wooden crate that stores things"         -> count 1   (right)
//   "a prison related sprite sheet"             -> count 1   (should be several)
//   "some shelves and barrels for a cellar"     -> count 2   (should be several)
//
// Two things are happening. The model is good at *categories* -- `kind` was correct on all
// five, including the case the design was written for -- and poor at *arithmetic*: asked
// for a weighted level index over a 1..8 scale it never left the bottom of it. And a
// request phrased as "a prison sheet" genuinely does not name a number, so the honest
// answer is a size class, not a count.
//
// `Choice` answers carry no confidence at all, which is the trade: a hedged number for a
// crisp class. A prompt that only needs a rough size is better served by the latter.
//
// The numbers are the bucket's typical size, and the set prompt uses them to say "about
// this many". `small` and `medium` are the sizes the design already proposed; `large` is a
// guess above them and is the one to change once a real large set has been generated.
inline constexpr int kSmallSet = 4;
inline constexpr int kMediumSet = 8;
inline constexpr int kLargeSet = 12;

// What the classifier was asked and what came back, in engine terms.
//
// Every field records "unanswered" distinctly from "answered low". The protocol returns no
// confidence at all for `Choice` answers, so a zero confidence here means "the model did not
// say" far more often than it means "the model was unsure", and a caller that conflates the
// two will invert the meaning of the field.
struct Verdict {
  // Null when the `kind` question was unanswered or its key was unknown. A caller must not
  // fall back to "the more likely kind"; the window keeps whatever mode it is already in
  // and says so.
  const SpriteKinds::Descriptor *kind = nullptr;

  // False when `shape` was unanswered. When false, `sheet` is false too: an unanswered
  // question resolves to the narrower case (one sprite), because a single-sprite prompt
  // against a set request produces one design the user can ask again for, while the
  // reverse produces a set nobody asked for.
  bool shapeAnswered = false;
  bool sheet = false;

  // The size class the model chose: "small", "medium" or "large". Empty when the `size`
  // question was unanswered.
  std::string sizeClass;

  // `sizeClass` as a number, for the prompt's "about this many". 1 for a single sprite,
  // and 0 when a set was asked for but no size came back -- the caller then has to decide,
  // and the window says so rather than inventing a number.
  int requested = 0;

  // Confidence, as reported. 0 means "the model did not say", which is *not* the same as
  // "unsure" -- and the two must be kept apart, because `SystemOneRecorder.h` documents
  // that some answer types carry no confidence field and that a missing one is written as 0
  // rather than defaulted to 1.
  //
  // Measured, and it corrects the assumption this was written with: hosted Jev *does*
  // report confidence on `Choice` answers. Real verdicts came back with kind 0.98/0.7,
  // shape 0.54 and size 0.22 -- so the "no confidence reported" case is a transport or
  // parse gap, not the norm. The doc comment on `Answer::confidence` saying it is absent on
  // some types still holds for those types; it is not true of every `Choice`.
  float kindConfidence = 0.0f;
  float shapeConfidence = 0.0f;
  float sizeConfidence = 0.0f;

  // The answers' own words, joined, for a window to show without parsing. Never interpreted:
  // `choice` is matched on its key, and this is only what a person reads.
  std::string why;

  // Every raw answer received, id and summary. Kept for the same reason the recorder keeps
  // records: when a verdict is wrong, the useful question is whether the model answered
  // wrongly or the parse lost the answer, and only the raw reply can tell them apart.
  struct RawAnswer {
    std::string id;
    std::string choice;
    float score = 0.0f;
    float probability = 0.0f;
    float confidence = 0.0f;
    bool hasChoice = false;
  };
  std::vector<RawAnswer> raw;

  // Why no verdict was reached, when there is none. Empty on a successful parse, including
  // a partial one -- a missing `kind` is not an error, it is an answer the caller must
  // handle.
  std::string error;

  // Whether the request reached the model at all. False on a transport failure, an HTTP
  // error or a timeout, and distinguishable from a model that answered nothing.
  bool asked = false;

  // True when the answer arrived but its confidence is below the active profile's
  // threshold. The verdict is otherwise complete, so a caller that only wants to *show*
  // what was read can still use it; one that wants to act should not.
  bool unsure = false;
};

using Callback = std::function<void(const Verdict &)>;

// The vocabulary the classifier is told about, so it can judge "a crate that holds things"
// as an object. Empty is allowed: the kind question then rests on the request's own words,
// which is worse but not broken.
struct Context {
  // The object capability names, as a comma-separated list. This is the same vocabulary the
  // object prompt is built with, from the same source (ObjectFactory), because a classifier
  // that believed a capability existed and a designer that could not write it would
  // together send every such request down the object path to be refused.
  std::string objectVocabulary;
};

// Builds the classifier config from the run's settings.
//
// Shared with `--systemone-test` rather than duplicated: the endpoint rule in particular is
// subtle (a `model.endpoint` is a *chat* endpoint, and posting a decisions body to one is
// how that test originally failed), so two copies would mean one of them getting it right.
SystemOne::JevClassifier::Config ConfigFromSettings();

// Asks the three questions. Never blocks: the request goes out on the classifier's own
// thread and `onDone` fires exactly once, including on transport failure, an HTTP error, an
// unparseable body and cancellation -- that guarantee is `SystemOneClassifier`'s and this
// preserves it.
//
// `text` is the user's request verbatim. Nothing is stripped or normalised: the classifier
// is being asked to read the same words the designer will, and a paraphrase here would be a
// decision made on different input than the one that follows.
// `minConfidence` is the active profile's threshold: below it the verdict is still
// parsed and reported, but flagged `unsure` so the caller does not act on it. 0 disables
// the check. It is a parameter rather than a setting read here because the threshold
// belongs to the *model*, and this function has no idea which model answered.
Verdict Parse(const std::vector<SystemOne::Answer> &answers, float minConfidence = 0.0f);

void Classify(SystemOne::SystemOneClassifier &classifier, const std::string &text,
              const Context &context, Callback onDone, std::stop_token stoken = {},
              float minConfidence = 0.0f);

// Runs one classification against a fresh classifier built from settings, prints a
// human-readable report and returns a process exit code. Used by `--sprite-route`, which
// exists so the wire format and the parse are verifiable with no window.
//
// Spends one small request, so it is never automatic.
int RunRouteTest(const std::string &text);

}  // namespace SpriteRouter
