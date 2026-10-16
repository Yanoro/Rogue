#include "TileArtQueue.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace {

// The worklist filename the game writes and the tool reads. One fixed name: there
// is only ever one batch in flight, and a name carrying a timestamp would leave
// the work directory to grow one file per batch.
constexpr const char *kWorklistName = "tile_art_worklist.json";

// Pipes a command and collects its exit status. Used instead of system() so the
// child's exit code is available -- "the tool failed" and "the tool could not be
// started" are different things to report.
int RunCommand(const std::string &command) {
  const int status = std::system(command.c_str());
  if (status == -1) return -1;
#ifdef WEXITSTATUS
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#else
  return status;
#endif
}

std::string ShellQuote(const std::string &text) {
  std::string out = "'";
  for (const char c : text) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out += c;
    }
  }
  out += "'";
  return out;
}

}  // namespace

TileArtQueue::~TileArtQueue() { Stop(); }

void TileArtQueue::Configure(const Config &newConfig,
                             const std::string &workDirectoryPath,
                             const std::string &spritesDirectoryPath,
                             const std::string &paletteJsonPath) {
  config = newConfig;
  workDirectory = workDirectoryPath;
  spritesDirectory = spritesDirectoryPath;
  paletteJson = paletteJsonPath;
}

void TileArtQueue::SetCompletion(CompletionFn fn) { completionFn = std::move(fn); }

bool TileArtQueue::Request(const std::string &tileName, std::string *reason) {
  auto refuse = [reason](const std::string &why) {
    if (reason != nullptr) *reason = why;
    return false;
  };

  if (tileName.empty()) {
    return refuse("there is no tile name to generate art for");
  }
  if (!config.enabled) {
    return refuse(
        "art generation is off. Turn on art.generation.enabled in settings.json "
        "(see SETTINGS.md); it spends money, so it is off by default.");
  }
  // Already handled, or already waiting: asking twice must not cost twice. `Done`
  // counts as handled even though the sprite could be regenerated -- if the owner
  // wants different art for a tile that has some, deleting the sprite first is the
  // way to say so.
  for (const Job &job : jobs) {
    if (job.tileName != tileName) continue;
    if (job.state == State::Queued || job.state == State::Running) {
      return refuse("'" + tileName + "' is already queued");
    }
    if (job.state == State::Done) {
      return refuse("'" + tileName + "' already has generated art");
    }
    break;
  }
  if (requestsUsed >= config.maxRequestsPerSession) {
    return refuse("the session limit of " +
                  std::to_string(config.maxRequestsPerSession) +
                  " generation request(s) is reached");
  }

  Job job;
  job.tileName = tileName;
  job.state = State::Queued;
  jobs.push_back(std::move(job));
  return true;
}

bool TileArtQueue::Busy() const {
  for (const Job &job : jobs) {
    if (job.state == State::Queued || job.state == State::Running) {
      return true;
    }
  }
  return false;
}

std::vector<TileArtQueue::Job> TileArtQueue::Snapshot() const {
  std::lock_guard<std::mutex> lock(pendingMutex);
  return jobs;
}

size_t TileArtQueue::CountInState(State state) const {
  std::lock_guard<std::mutex> lock(pendingMutex);
  size_t count = 0;
  for (const Job &job : jobs) {
    if (job.state == state) ++count;
  }
  return count;
}

bool TileArtQueue::Forget(const std::string &tileName) {
  std::lock_guard<std::mutex> lock(pendingMutex);
  for (auto it = jobs.begin(); it != jobs.end(); ++it) {
    if (it->tileName != tileName) continue;
    // Only before it has been paid for. A Running job's request is already out and
    // its file is on its way; dropping it would discard something already bought.
    if (it->state == State::Queued) {
      jobs.erase(it);
      return true;
    }
    return false;
  }
  return false;
}

void TileArtQueue::Clear() {
  std::lock_guard<std::mutex> lock(pendingMutex);
  // Keeps Running jobs: their worker is still going to post a completion, and a
  // completion with no matching job is exactly the "callback ran without a
  // corresponding request" error the AI path has to guard against.
  jobs.erase(std::remove_if(jobs.begin(), jobs.end(),
                            [](const Job &job) { return job.state != State::Running; }),
             jobs.end());
  queueAge = 0.0f;
}

bool TileArtQueue::Update(float deltaSeconds) {
  // Only one worker at a time: two batches in flight would each pay the per-request
  // cost and defeat the batching this queue exists for.
  {
    std::lock_guard<std::mutex> lock(workerMutex);
    if (workerRunning) return false;
  }

  bool anyQueued = false;
  {
    std::lock_guard<std::mutex> lock(pendingMutex);
    for (const Job &job : jobs) {
      if (job.state == State::Queued) {
        anyQueued = true;
        break;
      }
    }
  }
  if (!anyQueued) {
    queueAge = 0.0f;
    return false;
  }

  // Measured from the first request still waiting, so the window is "how long has
  // something been waiting" rather than "how long since the last one arrived" --
  // the latter would let a stream of arrivals postpone the batch forever.
  queueAge += deltaSeconds;
  if (queueAge < config.batchDelaySeconds) {
    return false;
  }
  queueAge = 0.0f;
  StartWorker();
  return true;
}

void TileArtQueue::StartWorker() {
  std::vector<std::string> batch;
  {
    std::lock_guard<std::mutex> lock(pendingMutex);
    for (Job &job : jobs) {
      if (job.state != State::Queued) continue;
      // Stopped, then started: a job beyond the cap stays Skipped rather than
      // sitting in Queued and being retried every batch.
      if (requestsUsed >= config.maxRequestsPerSession) {
        job.state = State::Skipped;
        job.error = "the session limit of " +
                    std::to_string(config.maxRequestsPerSession) +
                    " generation request(s) is reached";
        continue;
      }
      job.state = State::Running;
      batch.push_back(job.tileName);
    }
    if (batch.empty()) return;
    ++requestsUsed;
  }

  {
    std::lock_guard<std::mutex> lock(workerMutex);
    workerRunning = true;
  }
  // Detached and joined through the condition variable: the worker outlives the
  // frame that started it, so it cannot be owned by the call, but Stop() must still
  // be able to wait for it.
  std::thread([this, batch]() {
    RunBatch(batch);
    {
      std::lock_guard<std::mutex> lock(workerMutex);
      workerRunning = false;
    }
    workerDone.notify_all();
  }).detach();
}

void TileArtQueue::RunBatch(std::vector<std::string> tileNames) {
  Completion completion;
  completion.tiles = tileNames;

  std::error_code ec;
  if (workDirectory.empty()) {
    completion.error = "no work directory is configured for generation";
    std::lock_guard<std::mutex> lock(pendingMutex);
    pending.push_back(std::move(completion));
    return;
  }
  std::filesystem::create_directories(workDirectory, ec);
  if (ec) {
    completion.error = "could not create " + workDirectory + ": " + ec.message();
    std::lock_guard<std::mutex> lock(pendingMutex);
    pending.push_back(std::move(completion));
    return;
  }

  const std::filesystem::path worklist =
      std::filesystem::path(workDirectory) / kWorklistName;
  const std::filesystem::path results =
      std::filesystem::path(worklist.string() + ".result.json");

  // The result file is removed first. A stale one from a previous batch would
  // otherwise be read as this batch's outcome, and the tiles would be reported as
  // generated without anything having been generated.
  std::filesystem::remove(results, ec);

  {
    nlohmann::json request;
    request["tiles"] = tileNames;
    request["spritesDirectory"] = spritesDirectory;
    request["paletteJson"] = paletteJson;
    // Kept in the request so the tool's output is reproducible from the file alone,
    // which is what makes a failed batch debuggable after the fact.
    request["writtenBy"] = "TileArtQueue";
    std::ofstream out(worklist);
    if (!out.is_open()) {
      completion.error = "could not write " + worklist.string();
      std::lock_guard<std::mutex> lock(pendingMutex);
      pending.push_back(std::move(completion));
      return;
    }
    out << request.dump(2) << "\n";
  }

  std::string command = config.command;
  if (command.empty()) {
    completion.error =
        "no generation command is configured (art.generation.command)";
    std::lock_guard<std::mutex> lock(pendingMutex);
    pending.push_back(std::move(completion));
    return;
  }
  const size_t placeholder = command.find("{worklist}");
  if (placeholder != std::string::npos) {
    command.replace(placeholder, std::string("{worklist}").size(),
                    ShellQuote(worklist.string()));
  } else {
    command += " " + ShellQuote(worklist.string());
  }

  const int exitCode = RunCommand(command);

  // The result file is the contract, not the exit code: a tool that fails after
  // placing three of five tiles should report exactly that, and stdout is not a
  // place to put machine-readable state.
  nlohmann::json result;
  {
    std::ifstream in(results);
    if (!in.is_open()) {
      completion.error = "the generation command produced no result file (exit " +
                         std::to_string(exitCode) + "); see " +
                         worklist.string() + ".log if the tool wrote one";
      std::lock_guard<std::mutex> lock(pendingMutex);
      pending.push_back(std::move(completion));
      return;
    }
    try {
      in >> result;
    } catch (const std::exception &e) {
      completion.error = std::string("the result file is not valid JSON: ") +
                         e.what();
      std::lock_guard<std::mutex> lock(pendingMutex);
      pending.push_back(std::move(completion));
      return;
    }
  }

  completion.ok = result.value("ok", false);
  completion.error = result.value("error", std::string());
  completion.cost = result.value("cost", 0.0);

  std::lock_guard<std::mutex> lock(pendingMutex);
  pending.push_back(std::move(completion));
}

size_t TileArtQueue::DrainCompleted() {
  std::vector<Completion> finished;
  {
    std::lock_guard<std::mutex> lock(pendingMutex);
    finished.swap(pending);
  }
  if (finished.empty()) return 0;

  size_t handled = 0;
  for (Completion &completion : finished) {
    reportedCost += completion.cost;
    for (Job &job : jobs) {
      if (std::find(completion.tiles.begin(), completion.tiles.end(),
                    job.tileName) == completion.tiles.end()) {
        continue;
      }
      if (job.state != State::Running) continue;

      if (completion.ok) {
        job.state = State::Done;
        job.error.clear();
      } else {
        job.state = State::Failed;
        job.error = completion.error.empty() ? "generation failed"
                                             : completion.error;
      }
      ++handled;

      // The swap happens here, on the main thread, one job at a time. The queue
      // does not know what a sprite is; the callback is Game's.
      if (completionFn) {
        completionFn(job);
      }
    }
  }
  return handled;
}

void TileArtQueue::Stop() {
  {
    std::unique_lock<std::mutex> lock(workerMutex);
    if (!workerRunning) {
      stopping = true;
      return;
    }
    stopping = true;
    // Bounded wait. A request can legitimately take tens of seconds, and a game
    // that will not close for thirty seconds is its own bug -- so the wait is
    // generous but not unbounded, and a timeout is reported by leaving the worker
    // to finish on its own (the process is ending, and the completion it posts
    // refers to nothing).
    workerDone.wait_for(lock, std::chrono::seconds(30),
                        [this]() { return !workerRunning; });
  }
}
