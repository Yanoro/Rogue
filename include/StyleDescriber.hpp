#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class AI;
class DebugLog;

// Reads the art around a gap and says what it looks like, in the terms the sprite
// DSL can actually use.
//
// This is route "b" of the two ways a designing model can be shown the building it is
// drawing for: a separate vision call turns the crop into a short, checkable record,
// and the *designing* model stays text-only. The alternative -- sending the crop
// straight to the document turn -- is better information and a worse trade today,
// because the designer backend is text-only by construction
// (the designer's own comment says so) and a provider is allowed to ignore an
// image list silently. A no-op is the one failure mode worth paying a hop to avoid.
//
// What this deliberately does NOT do is describe the gap. An image model looking at a
// question mark in a wall will happily invent "a barred alcove"; the prompt forbids
// it and ParseReply refuses nothing about it, which is why the record is kept small
// and factual rather than prose. The fields are the ones that survive from a picture
// to a DSL document: materials, tones, light, density, edges.
namespace StyleDescriber {

// Where the instructions live. A file rather than a string, for the same reason the
// other designers' prompts are files: iterating on the wording must not need a build.
inline constexpr const char *kDefaultPromptPath = "data/prompts/STYLE_DESC.md";

// What the crop looked like, as facts. `summary` is the one paragraph that goes into
// SpriteDocumentBrief::surroundings; the fields exist so a caller can log or test
// them without re-parsing the paragraph.
struct Style {
  bool ok = false;
  // Blocking reason. Empty when ok. Worded to be logged, not sent back to a model:
  // unlike the designers there is no retry loop here, because a style brief is an
  // improvement and never a precondition.
  std::string error;
  // The reply verbatim, kept for the log. An empty style with a raw reply is exactly
  // the case worth reading by hand.
  std::string raw;
  std::vector<std::string> materials;
  std::vector<std::string> tones;
  std::string light;
  std::string density;
  std::string edges;
  // The record as prose, for the document turn's prompt.
  std::string summary;
};

// Turns one reply into a Style. Pure and model-free, so the grammar can be exercised
// without a backend -- `--blueprint-snapshot --describe` with a canned reply is the
// deterministic half.
Style ParseReply(const std::string &reply);

// One describer request at a time, mirroring MemoryDigestJob: a worker thread asks the
// vision backend, the main thread polls for the answer, and completion is the empty
// token sentinel rather than generateStream returning. One in flight because the two
// useful things to do with a second concurrent vision call are nothing.
class Job {
public:
  Job() = default;
  ~Job();

  Job(const Job &) = delete;
  Job &operator=(const Job &) = delete;

  // `ai` is borrowed and may be null, in which case the job reports unavailable
  // rather than offering a request that cannot finish.
  void Configure(AI *ai, DebugLog *log);

  bool Available() const { return ai != nullptr; }
  bool Busy() const;

  // Starts reading `imagePngBase64`. `context` is what the image is of -- the
  // snapshot's own description -- so the model is not guessing at what it is shown.
  // Non-blocking. False with a reason when unavailable or already busy.
  bool Request(const std::string &imagePngBase64, const std::string &context,
               std::string *reason = nullptr);

  // Moves a finished result out. False when nothing has finished yet. Safe to call
  // every frame, which is how the window drains it.
  bool Poll(Style *out);

  // Blocks until a result arrives or the timeout expires. For headless callers, which
  // have no frame to return to.
  bool WaitFor(Style *out, int timeoutMs);

  // Abandons the request in flight. The worker still finishes its HTTP call; the
  // result is dropped rather than delivered, because the caller has moved on.
  void Cancel();

private:
  // Everything a provider callback touches, owned by the *threads* rather than by the
  // Job.
  //
  // This is not indirection for its own sake. OllamaAI dispatches its own detached curl
  // thread and invokes the stream callback from it, so the callback can fire after the
  // worker has returned and after the Job has been destroyed -- which is exactly what an
  // earlier version did, producing a heap-buffer-overflow inside the callback. Capturing
  // a shared state object by value keeps the memory valid for as long as any callback can
  // still reach it; the late tokens are then simply dropped.
  struct Stream;

  void Run(std::string base64, std::string context, std::shared_ptr<Stream> stream);

  AI *ai = nullptr;
  DebugLog *log = nullptr;
  std::string instructions;
  std::string contextId = "style-describer";

  // The run in flight, reachable by Cancel() and by the worker. Null between runs.
  std::shared_ptr<Stream> stream;

  // Result delivery. Separate from Stream because only these two are the Job's own
  // lifetime: the worker stores, the main thread polls.
  mutable std::mutex mutex;
  std::condition_variable deliver;
  std::thread worker;
  bool busy = false;
  bool hasResult = false;
  // Set by Cancel so a headless caller blocked in WaitFor returns instead of waiting out
  // the timeout for a result that will never come.
  bool abandoned = false;
  Style result;
};

}  // namespace StyleDescriber
