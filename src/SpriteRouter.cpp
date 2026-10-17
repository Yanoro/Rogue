#include "SpriteRouter.hpp"
#include "SystemOneProfiles.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <sstream>

#include "ObjectFactory.h"
#include "Settings.h"

namespace SpriteRouter {

namespace {

// The question ids. Stable strings because they are what the answers are keyed by, and a
// reply that comes back in another order must still be attributable.
constexpr const char *kKindQuestion = "kind";
constexpr const char *kShapeQuestion = "shape";
constexpr const char *kSizeQuestion = "size";

// A size class as a number. Unknown or empty is 0, which the caller must handle rather
// than treating as one -- a set with no size is a question the window has to answer, not a
// silent single sprite.
int SizeClassToCount(const std::string &sizeClass) {
  if (sizeClass == "small") return kSmallSet;
  if (sizeClass == "medium") return kMediumSet;
  if (sizeClass == "large") return kLargeSet;
  return 0;
}

const SystemOne::Answer *Find(const std::vector<SystemOne::Answer> &answers,
                              const std::string &id) {
  for (const SystemOne::Answer &answer : answers) {
    if (answer.id == id) return &answer;
  }
  return nullptr;
}

}  // namespace

SystemOne::JevClassifier::Config ConfigFromSettings() {
  const GameSettings &settings = Settings();

  // Delegated rather than re-derived. The endpoint rule here was subtle enough that two
  // copies would mean one of them getting it wrong -- and with profiles there would
  // now be a third place (the picker) needing the same answer. One resolver, three
  // callers.
  const std::vector<SystemOne::ModelProfile> profiles =
      SystemOne::ResolveProfiles(settings);
  const std::string active = SystemOne::ResolveActiveId(profiles, settings, "",
                                                        settings.system1.cliProfile);
  if (const SystemOne::ModelProfile *profile =
          SystemOne::FindProfile(profiles, active)) {
    return profile->config;
  }
  // Unreachable in practice: ResolveProfiles always returns at least "default". Kept
  // as an explicit fallback rather than a dereference so a future resolver bug cannot
  // become a crash.
  return SystemOne::JevClassifier::Preset::HostedJev();
}

Verdict Parse(const std::vector<SystemOne::Answer> &answers, float minConfidence) {
  Verdict verdict;
  verdict.asked = true;
  verdict.raw.reserve(answers.size());

  std::vector<std::string> why;

  for (const SystemOne::Answer &answer : answers) {
    Verdict::RawAnswer raw;
    raw.id = answer.id;
    raw.choice = answer.choice;
    raw.score = answer.score;
    raw.probability = answer.probability;
    raw.confidence = answer.confidence;
    raw.hasChoice = !answer.choice.empty();
    verdict.raw.push_back(std::move(raw));
  }

  // ---- kind ---------------------------------------------------------------------
  if (const SystemOne::Answer *answer = Find(answers, kKindQuestion)) {
    // A `choice` answer is the option's *key*, chosen by the caller -- not an index, and not
    // necessarily one of ours. `Parse` returns null for a key this build does not know,
    // which is a state the caller resolves; a `static_cast` would turn an unknown key into
    // an arbitrary kind.
    verdict.kind = SpriteKinds::Parse(answer->choice);
    verdict.kindConfidence = answer->confidence;
    if (!answer->choice.empty()) {
      why.push_back("kind=" + answer->choice +
                    (answer->confidence > 0.0f
                         ? " (confidence " + std::to_string(answer->confidence) + ")"
                         : std::string(" (no confidence reported)")));
    }
    if (verdict.kind == nullptr && !answer->choice.empty()) {
      // Not an error the caller has to fix by editing the classifier: the model named a key
      // from a newer build, or something that is not one of ours at all. Reported so the
      // dropdown can say "the classifier said 'item', which this build does not have".
      verdict.error = "the classifier chose an unknown kind '" + answer->choice + "'";
    }
  }

  // ---- shape --------------------------------------------------------------------
  if (const SystemOne::Answer *answer = Find(answers, kShapeQuestion)) {
    if (answer->choice == "single") {
      verdict.shapeAnswered = true;
      verdict.sheet = false;
    } else if (answer->choice == "set") {
      verdict.shapeAnswered = true;
      verdict.sheet = true;
    }
    verdict.shapeConfidence = answer->confidence;
    why.push_back("shape=" +
                  (answer->choice.empty() ? std::string("(nothing)") : answer->choice) +
                  (answer->confidence > 0.0f
                       ? " (confidence " + std::to_string(answer->confidence) + ")"
                       : std::string(" (no confidence reported)")));
  }

  // ---- size ---------------------------------------------------------------------
  if (const SystemOne::Answer *answer = Find(answers, kSizeQuestion)) {
    verdict.sizeClass = answer->choice;
    verdict.sizeConfidence = answer->confidence;
    why.push_back("size=" +
                  (answer->choice.empty() ? std::string("(nothing)") : answer->choice) +
                  (answer->confidence > 0.0f
                       ? " (confidence " + std::to_string(answer->confidence) + ")"
                       : std::string(" (no confidence reported)")));
  }

  // A single sprite is one sprite whatever the size question said, so that answer is only
  // read for a set. This is also why the two questions are asked rather than inferred from
  // each other: "a set of one" is not a shape anyone means.
  if (!verdict.sheet) {
    verdict.requested = 1;
    verdict.sizeClass.clear();
  } else {
    verdict.requested = SizeClassToCount(verdict.sizeClass);
  }

  // The per-model confidence floor. Below it the verdict is still reported in full --
  // the raw answers are what a person needs to judge the threshold itself -- but flagged
  // so the caller keeps whatever it was doing instead of acting on a coin flip.
  //
  // Compared against the `kind` confidence because `kind` is the answer the caller acts
  // on: shape and size are refinements, and a certain kind with a doubtful size is still
  // worth using. Choice answers carry no confidence at all on some backends, in which case
  // the value is 0 and this never fires -- which is why the threshold defaults to 0.
  if (minConfidence > 0.0f && verdict.kind != nullptr &&
      verdict.kindConfidence < minConfidence) {
    verdict.unsure = true;
    why.push_back("confidence " + std::to_string(verdict.kindConfidence) +
                  " is below the " + std::to_string(minConfidence) + " threshold");
  }

  // Joined for display. Not parsed by anything.
  for (size_t i = 0; i < why.size(); ++i) {
    if (i > 0) verdict.why += "; ";
    verdict.why += why[i];
  }
  return verdict;
}

void Classify(SystemOne::SystemOneClassifier &classifier, const std::string &text,
              const Context &context, Callback onDone, std::stop_token stoken,
              float minConfidence) {
  // `state` is the engine's own description of the situation, never chat history. The
  // request text is quoted rather than embedded, so a request containing a line that looks
  // like a label cannot be mistaken for one.
  std::string state = "A person has asked the sprite designer for the following:\n\n" +
                      text + "\n";
  if (!context.objectVocabulary.empty()) {
    state += "\nThe object capabilities this engine has are: " + context.objectVocabulary +
             ".\nAn object is a thing made of one or more of those capabilities -- something\n"
             "that can be used, carried, opened, harvested, crafted at or grown. A tile is\n"
             "one square of terrain with no capabilities: a floor, a wall, a patch of grass,\n"
             "water, a path. \"It stores things\" makes something an object even though a\n"
             "crate is also furniture.\n";
  }
  state += "\nAnswer about the request itself, not about what you would design.";

  std::vector<SystemOne::Option> questions;

  SystemOne::Option kind;
  kind.id = kKindQuestion;
  kind.type = SystemOne::OptionType::Choice;
  kind.instructions = "Is this request for one or more tiles, or one or more objects?";
  // The keys are the engine's, not the model's: they are what `SpriteKinds::Parse` matches,
  // and the protocol lets the caller name them precisely so a reply does not depend on the
  // model spelling anything a particular way.
  kind.options = {{"tile", "ground only: a floor, path, grass, water or other square of terrain"},
                  {"object", "a thing with a capability: a wall, door, usable, carryable, "
                             "openable, harvestable, craftable or growing"}};
  questions.push_back(std::move(kind));

  SystemOne::Option shape;
  shape.id = kShapeQuestion;
  shape.type = SystemOne::OptionType::Choice;
  shape.instructions =
      "Does the request describe a single sprite, or a set of several?";
  shape.options = {
      {"single", "one thing, named or described as one"},
      {"set", "several things: a themed group, a room's contents, a family of variants"}};
  questions.push_back(std::move(shape));

  // A size *class*, not a number. See kSmallSet in the header for the measurements that
  // forced this: asked for a count on a 1..8 scale, the model never left the bottom of it,
  // while it classified `kind` correctly on every real request including the hard one.
  SystemOne::Option size;
  size.id = kSizeQuestion;
  size.type = SystemOne::OptionType::Choice;
  size.instructions =
      "If this is a set, how large a set? A few pieces, a room's worth, or a whole "
      "building's worth. If it is a single sprite, answer small.";
  size.options = {
      {"small", "one or two things, or a single sprite"},
      {"medium", "a handful: the contents of one room"},
      {"large", "many: a whole building or location's worth"}};
  questions.push_back(std::move(size));

  // The callback always fires exactly once: `SystemOneClassifier` guarantees it for transport
  // failure, HTTP error, unparseable body and cancellation, and `Parse` is total -- it
  // returns a verdict with no kind rather than throwing. So there is no second call to guard
  // against here.
  SystemOne::SystemOneClassifier::Callback done =
      [onDone = std::move(onDone), minConfidence](
          const std::vector<SystemOne::Answer> &answers,
          const SystemOne::CallTrace &trace) {
        Verdict verdict = Parse(answers, minConfidence);
        if (!trace.Ok() && verdict.error.empty()) {
          verdict.error = trace.error;
        }
        // `asked` records whether the request left the process, which is a different
        // question from whether it was answered: a transport failure is worth distinguishing
        // from a model that replied with nothing, because only one of them is worth retrying.
        verdict.asked = trace.Ok() || verdict.asked;
        if (onDone) onDone(verdict);
      };

  SystemOne::Attribution attribution;
  attribution.scenario = "sprite_route";
  // One line for the record list. The request itself, truncated: the record is for judging
  // the classifier's decisions, and a decision is only interpretable next to what was asked.
  attribution.summary = text.size() > 80 ? text.substr(0, 77) + "..." : text;
  classifier.Ask(state, questions, std::move(done), stoken, attribution);
}

int RunRouteTest(const std::string &text) {
  const GameSettings &settings = Settings();

  SystemOne::JevClassifier classifier(ConfigFromSettings());
  if (!classifier.Available()) {
    std::cerr << "sprite-route: no API key in system1.model.apiKey or model.apiKey, so "
                 "there is nothing to ask\n";
    return 1;
  }

  // The object vocabulary is the classifier's only way to judge "a crate that holds
  // things". Built from the factory, the same source the object prompt uses, so a
  // capability the classifier believes in is one the designer can write.
  ObjectFactory objects;
  Context context;
  {
    std::string list;
    for (const std::string &name : objects.ComponentNames()) {
      if (!list.empty()) list += ", ";
      list += name;
    }
    context.objectVocabulary = list;
  }

  // The active profile, so this harness reports which backend it used and applies that
  // backend's confidence floor -- the same two things the window does. Without this the
  // threshold would be unverifiable outside the GUI, and a GUI-only feature is
  // unreachable to this repo's kind of testing.
  const std::vector<SystemOne::ModelProfile> profiles =
      SystemOne::ResolveProfiles(settings);
  const SystemOne::ModelProfile *profile = SystemOne::FindProfile(
      profiles,
      SystemOne::ResolveActiveId(profiles, settings, "", settings.system1.cliProfile));
  const float minConfidence = profile != nullptr ? profile->minConfidence : 0.0f;

  std::cout << "routing:    " << text << "\n";
  std::cout << "profile:    " << (profile != nullptr ? profile->id : "(none)")
            << "  threshold " << minConfidence << "\n";
  std::cout << "classifier: " << classifier.ModelLabel() << " via "
            << classifier.Endpoint() << "\n";
  std::cout << "capabilities: " << (context.objectVocabulary.empty()
                                        ? std::string("(none)")
                                        : context.objectVocabulary)
            << "\n";

  // Completion is signalled by this callback, never by `Ask` returning: the provider's
  // request runs on its own thread and `Ask` posts and returns. Waiting on `Ask` is the bug
  // that made an earlier rewriter report "the model returned nothing" every time.
  std::mutex mutex;
  std::condition_variable done;
  bool finished = false;
  Verdict verdict;

  std::stop_source stop;
  Classify(classifier, text, context,
           [&](const Verdict &result) {
             std::lock_guard<std::mutex> lock(mutex);
             verdict = result;
             finished = true;
             done.notify_all();
           },
           stop.get_token(), minConfidence);

  {
    std::unique_lock<std::mutex> lock(mutex);
    // Bounded generously: the provider has its own timeout and always fires the callback, so
    // this wait is a backstop against a provider that breaks that contract, not the expected
    // path. Five minutes is far past any decision call.
    if (!done.wait_for(lock, std::chrono::minutes(5), [&] { return finished; })) {
      std::cerr << "sprite-route: the classifier never reported back\n";
      return 1;
    }
  }

  if (!verdict.error.empty()) {
    std::cout << "error:      " << verdict.error << "\n";
  }
  std::cout << "verdict:\n";
  std::cout << "  kind:     "
            << (verdict.kind != nullptr ? verdict.kind->id : std::string("(unanswered)"))
            << "  confidence " << verdict.kindConfidence << "\n";
  std::cout << "  shape:    "
            << (verdict.shapeAnswered ? (verdict.sheet ? "set" : "single")
                                      : std::string("(unanswered)"))
            << "  confidence " << verdict.shapeConfidence << "\n";
  std::cout << "  size:     "
            << (verdict.sizeClass.empty() ? std::string("(unanswered)")
                                          : verdict.sizeClass)
            << "  -> about " << verdict.requested << " sprite(s)\n";
  std::cout << "  why:      " << (verdict.why.empty() ? "(nothing)" : verdict.why) << "\n";

  // The raw answers, printed unconditionally. This is the mode's whole point for the count
  // question: `Value` answers are the ones most likely to be unusable, and whether the model
  // answered wrongly or the parse lost it can only be told apart from the raw reply.
  std::cout << "raw answers (" << verdict.raw.size() << "):\n";
  for (const Verdict::RawAnswer &raw : verdict.raw) {
    std::cout << "  " << raw.id << ": ";
    if (raw.hasChoice) std::cout << "choice=" << raw.choice << " ";
    std::cout << "score=" << raw.score << " probability=" << raw.probability
              << " confidence=" << raw.confidence << "\n";
  }

  if (verdict.kind == nullptr) {
    std::cout << "RESULT:     no kind, so the caller resolves it (the window keeps its "
                 "current mode)\n";
    return 1;
  }
  if (verdict.unsure) {
    // Reported as its own outcome rather than as the verdict, because that is what the
    // game does with it: below the active profile's threshold the window keeps the mode
    // it was already in. A harness that printed the verdict here would make an ignored
    // answer look like an applied one.
    std::cout << "RESULT:     unsure (" << verdict.kind->id
              << " below the threshold), so the caller keeps its current mode\n";
    return 0;
  }
  std::cout << "RESULT:     " << verdict.kind->id << ", "
            << (verdict.sheet ? (verdict.sizeClass.empty()
                                     ? std::string("a set (size unanswered)")
                                     : "a " + verdict.sizeClass + " set (~" +
                                           std::to_string(verdict.requested) + ")")
                              : std::string("a single"))
            << "\n";
  return 0;
}

}  // namespace SpriteRouter
