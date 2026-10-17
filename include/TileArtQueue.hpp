#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// The live half of tile art generation: a worker thread produces sprites, a
// main-thread drain swaps them in, and the frame in between is never blocked.
//
// This is the queue docs/asset-generation-design.md §10.2 and
// docs/tile-art-wiring-design.md §4 describe, built for tiles. It mirrors the AI
// path's shape -- background thread produces, a mutex-guarded queue hands the
// result to the main thread, which is the only place anything is mutated -- and it
// deliberately does not copy its *storage*: `g_AIResponseMutex` /
// `g_AIResponseQueue` are file-scope process globals, which `.agents/known-issues.md`
// #13 flags (they break the moment a second `flecs::world` exists). This queue is a
// member of `Game` instead, so it is per-world by construction. The AI path was not
// changed: it works, and touching it is not this step's job.
//
// Why a separate thread and not the frame loop: a request is a network round trip
// of seconds to tens of seconds (§10.1 measured 10-30s locally). In the frame loop
// that stalls the game for a minute to produce something that may be rejected. The
// glyph is already a legitimate rendering of the tile (phase 1), so the tile is
// usable from the instant it is designed and the art is a later improvement --
// there is never a reason to make the player wait for it.
//
// **Batching is the point, not an optimisation.** One request costs the same
// whether it carries one tile or nineteen (~1,290 image tokens, §6), so
// `batchDelaySeconds` collects everything requested in a short window into a single
// request. Draining one tile at a time would be roughly 19x the cost per asset --
// which is exactly the kind of thing nobody notices until the bill arrives.
class TileArtQueue {
public:
  enum class State {
    Queued,   // accepted, waiting for the batch window to close
    Running,  // a worker is generating this batch
    Done,     // the sprite landed and was swapped in
    Failed,   // generation or placement failed; `error` says why
    Skipped,  // never sent -- the session cap was already reached
  };

  struct Job {
    std::string tileName;
    State state = State::Queued;
    std::string error;
    // The sprite that was placed, when it was.
    std::string path;
    // Map cells that gained a sprite on the swap, so the window can say what
    // actually changed rather than only that something did.
    size_t refreshedCells = 0;
    // Cells whose visual the swap updated. Set by the completion callback, so the
    // count the window and the logs report is the one the swap actually achieved
    // rather than a re-measurement that would also count work a later frame did.
    size_t swappedCells = 0;
    // The worklist this tile was generated through, once a batch has been sent.
    // Recorded because everything the pipeline produced hangs off it by
    // convention: the raw sheet is `<worklist's directory>/tile_art_sheet.png`, its
    // prompt is the same path with `.prompt.txt`, and the per-stage cell images are
    // `cell_<index>_{crop,trim,locked}.png` beside them. Without this the window has
    // no way back to what the model actually drew, which is the only thing that
    // explains a bad tile.
    std::string worklistPath;
  };

  struct Config {
    // Off by default. This is the one setting in the feature that spends money
    // without a human present, so turning it on is an explicit act.
    bool enabled = false;
    // A command that turns a worklist JSON into sprites. `{worklist}` is replaced
    // with the path this queue writes; the command writes progress and results to
    // `<worklist>.result.json`. Empty means "use the built-in default", which is
    // tools/gen_tile_art.py through nix-shell -- see Settings.h for what the
    // default looks like and why it is a command rather than a linked-in backend.
    std::string command;
    // How long to collect requests before spending a request on them. Small
    // enough that a single tile is not noticeably delayed, large enough that a
    // designer creating three tiles in a row pays for one request instead of
    // three.
    float batchDelaySeconds = 2.0f;
    // The hard ceiling on requests for the session. A request is ~$0.04, and an
    // agent that decides to invent forty tiles should not be able to run up a bill
    // unattended. Counted in requests, not tiles, because requests are what cost.
    int maxRequestsPerSession = 8;
  };

  // Called on the main thread once per settled job, in the order results arrive.
  // This is where the swap happens: the queue knows nothing about the ECS.
  //
  // A `std::function` rather than a raw pointer so the owner can bind its own
  // context -- `g_AIResponseQueue` stores lambdas for the same reason. The job
  // arrives by const reference and is only valid for the call: a callback that
  // wants to keep it must copy it.
  //
  // Returns how many cells the swap touched, which the queue records on the job so a
  // caller can tell "nothing to swap yet" from "the swap failed". Zero is the normal
  // answer for a tile that has been designed and not yet painted.
  using CompletionFn = std::function<size_t(const Job &job)>;

  TileArtQueue() = default;
  ~TileArtQueue();

  TileArtQueue(const TileArtQueue &) = delete;
  TileArtQueue &operator=(const TileArtQueue &) = delete;

  // Copies the settings into the queue. Main thread, before or between frames.
  void Configure(const Config &config, const std::string &workDirectory,
                 const std::string &spritesDirectory,
                 const std::string &paletteJson);

  // Why art cannot be generated at all, or empty when it can. The same conditions
  // `Request` refuses on, asked ahead of time, so a surface can say what is missing
  // instead of only reporting that nothing happened.
  //
  // This exists because the failure mode of a disabled feature is silence: a tile
  // is accepted, no sprite appears, and nothing anywhere says why. Every reason
  // here is actionable, which is the bar for putting it in front of someone.
  std::string UnavailableReason() const;

  const Config &GetConfig() const { return config; }
  bool Enabled() const { return config.enabled; }
  // Requests already spent. The window shows this next to the cap so the cost is
  // visible rather than inferred.
  int RequestsUsed() const { return requestsUsed; }
  // The API's own reported cost, summed, when the tool reported one.
  double ReportedCost() const { return reportedCost; }

  // Registers the completion callback. Set once, by Game, before anything can be
  // queued.
  void SetCompletion(CompletionFn fn);

  // Adds one tile to the next batch. Returns false when generation is disabled, the
  // name is empty, the tile is already queued or done, or the session cap is
  // reached -- in which case `reason` says which.
  bool Request(const std::string &tileName, std::string *reason = nullptr);

  // True while anything is waiting or in flight, so a caller can show "working".
  bool Busy() const;

  // Copies the current job list under the lock, for the UI. Cheap and called per
  // frame, which is why the jobs are only ever mutated on the main thread: the
  // worker never touches `jobs`, it posts to `pending` instead.
  std::vector<Job> Snapshot() const;

  // The main-thread drain, called once per frame by the ECS system. Runs the
  // completion callback for every settled job. Returns how many it handled.
  size_t DrainCompleted();

  // Called once per frame, after the drain. Starts a worker when the batch window
  // has elapsed. Returns true when it started one.
  bool Update(float deltaSeconds);

  // Marks a tile as no longer interesting (the design was deleted). Only drops it
  // while it is still Queued: once a batch is in flight the money is spent, and
  // cancelling would throw away a file that has already been paid for.
  bool Forget(const std::string &tileName);

  // Forgets every job, for a map reload. Does not stop an in-flight batch.
  void Clear();

  // Joins the worker. Called from Game's destructor (and thus Shutdown); without
  // it a batch in flight would outlive the world its completion callback refers to.
  void Stop();

  // How many jobs exist per state, for a status line.
  size_t CountInState(State state) const;

private:
  // The message a worker posts to the main thread. Deliberately data, not a
  // lambda closed over world state: a lambda would carry an `flecs::entity` or a
  // `Game*` across the thread boundary, which is how the AI path's callbacks ended
  // up needing `entity.is_alive()` guards. The worker here knows nothing it could
  // capture.
  struct Completion {
    std::vector<std::string> tiles;
    bool ok = false;
    std::string error;
    size_t refreshedCells = 0;
    double cost = 0.0;
  };

  void StartWorker();
  void RunBatch(std::vector<std::string> tileNames);

  Config config;
  std::string workDirectory;
  std::string spritesDirectory;
  std::string paletteJson;

  // Main thread only. No mutex: `DrainCompleted` and the UI both run on the main
  // thread, so guarding them would be guarding nothing, and the worker is kept out
  // entirely by never touching this.
  std::vector<Job> jobs;
  // Seconds accumulated since the first currently-queued request arrived, so the
  // batch window is measured from the first request rather than from the last.
  float queueAge = 0.0f;
  int requestsUsed = 0;
  double reportedCost = 0.0;

  // The thread boundary. `pending` is written by workers and read by the main
  // thread under `pendingMutex`; the condition variable is only used by Stop, so a
  // worker parked mid-shutdown can be waited on rather than detached.
  mutable std::mutex pendingMutex;
  std::vector<Completion> pending;

  std::mutex workerMutex;
  std::condition_variable workerDone;
  std::thread worker;
  bool workerRunning = false;
  bool stopping = false;

  CompletionFn completionFn;
};
