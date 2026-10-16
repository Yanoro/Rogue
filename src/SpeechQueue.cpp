#include "SpeechQueue.h"

#include <algorithm>
#include <cstring>
#include <dlfcn.h>
#include <utility>

namespace {

// How much speech may be waiting to be synthesised. The designer needs one
// utterance at a time; the cap exists so a future caller cannot turn the queue into
// an unbounded backlog of stale commentary.
constexpr size_t kMaxQueuedJobs = 3;

// A canonical 44-byte PCM WAV header followed by the samples.
//
// Built by hand rather than through raylib because the samples are already decoded
// PCM and the only thing missing is the header a Music stream needs in order to be
// seekable. Mono, 16-bit, which is what every engine here produces.
std::vector<uint8_t> MakeWav(const std::vector<int16_t> &samples, int sampleRate) {
  const uint32_t dataBytes = static_cast<uint32_t>(samples.size() * sizeof(int16_t));
  const uint16_t channels = 1;
  const uint16_t bitsPerSample = 16;
  const uint32_t byteRate =
      static_cast<uint32_t>(sampleRate) * channels * (bitsPerSample / 8);
  const uint16_t blockAlign = static_cast<uint16_t>(channels * (bitsPerSample / 8));

  std::vector<uint8_t> wav(44 + dataBytes);
  auto put32 = [&wav](size_t offset, uint32_t value) {
    std::memcpy(wav.data() + offset, &value, sizeof(value));
  };
  auto put16 = [&wav](size_t offset, uint16_t value) {
    std::memcpy(wav.data() + offset, &value, sizeof(value));
  };

  std::memcpy(wav.data() + 0, "RIFF", 4);
  put32(4, 36 + dataBytes);
  std::memcpy(wav.data() + 8, "WAVE", 4);
  std::memcpy(wav.data() + 12, "fmt ", 4);
  put32(16, 16);  // PCM fmt chunk size
  put16(20, 1);   // format: PCM
  put16(22, channels);
  put32(24, static_cast<uint32_t>(sampleRate));
  put32(28, byteRate);
  put16(32, blockAlign);
  put16(34, bitsPerSample);
  std::memcpy(wav.data() + 36, "data", 4);
  put32(40, dataBytes);
  if (dataBytes > 0) {
    std::memcpy(wav.data() + 44, samples.data(), dataBytes);
  }
  return wav;
}

// Whether any of the libraries miniaudio would use as a real backend can be
// loaded.
//
// This exists because miniaudio falls back to a NULL device when none of them
// load, and that fallback is invisible: InitAudioDevice() logs success,
// IsAudioDeviceReady() returns true, playback runs and IsSoundPlaying() goes true
// then false after exactly the right duration -- with no sound, ever. The check is
// cheap and the alternative is silence that reports itself as working.
//
// RTLD_NOLOAD first, so an already-loaded library (the ones CMakeLists links
// explicitly) is detected without bumping its refcount, and a plain dlopen second
// as the fallback for a build that did not link them.
bool RealAudioBackendLoadable() {
  static const char *const kCandidates[] = {
      "libpulse.so.0",   // preferred by miniaudio when present
      "libasound.so.2",  // ALSA, which is what this machine's PipeWire exposes
      "libjack.so.0",
      "libsndio.so.7",
  };

  for (const char *name : kCandidates) {
    if (void *handle = dlopen(name, RTLD_LAZY | RTLD_NOLOAD)) {
      dlclose(handle);
      return true;
    }
    if (void *handle = dlopen(name, RTLD_LAZY)) {
      dlclose(handle);
      return true;
    }
  }
  return false;
}

}  // namespace

SpeechQueue::SpeechQueue(TextToSpeech *backend, float volume)
    : backend(backend), volume(volume) {}

SpeechQueue::~SpeechQueue() { Shutdown(); }

bool SpeechQueue::Init() {
  sampleRate = backend != nullptr ? backend->SampleRate() : 22050;

  ready = IsAudioDeviceReady();
  if (!ready) {
    audioWarning =
        "the audio device could not be opened, so nothing will be spoken.";
    return false;
  }

  if (!RealAudioBackendLoadable()) {
    // Deliberately still "ready": the engine runs and the pipeline is exercised,
    // and refusing to speak would hide the problem behind a different symptom. The
    // warning is what the window shows, and it is the truth raylib will not tell us.
    audioWarning =
        "no audio backend library (libpulse or libasound) could be loaded, so "
        "raylib fell back to a silent null device. Speech will run but produce no "
        "sound. Run the game from nix-shell (./compile_and_run.sh), which puts the "
        "audio libraries on the loader path.";
  }

  worker = std::thread([this] { Worker(); });
  return true;
}

void SpeechQueue::Shutdown() {
  quitting = true;
  stopRequested = true;
  autoplayId = kNoSpeechClip;
  queueCv.notify_all();

  if (worker.joinable()) {
    // The worker may be parked inside a blocking Speak(). Killing the child is what
    // unblocks it, so this has to happen before the join, not after.
    if (backend != nullptr) {
      backend->Stop();
    }
    worker.join();
  }

  UnloadMusic();

  {
    std::lock_guard<std::mutex> lock(queueMutex);
    queue.clear();
  }
  {
    std::lock_guard<std::mutex> lock(publishMutex);
    published.clear();
  }
  clips.clear();
  ready = false;
}

SpeechClipId SpeechQueue::Enqueue(std::string text) {
  if (backend == nullptr || !ready || muted.load() || text.empty()) {
    return kNoSpeechClip;
  }

  const SpeechClipId id = nextId++;
  Job job{id, std::move(text)};

  // The clip deliberately does not keep a copy of the text: the window holds it,
  // because the window is what offers to speak it again after the audio is evicted.
  Clip clip;
  clip.id = id;
  clips.emplace(id, std::move(clip));

  {
    std::lock_guard<std::mutex> lock(queueMutex);
    // A pending Stop must not swallow the next utterance: clearing the flag here is
    // what lets "stop talking, now say this" work.
    stopRequested = false;
    while (queue.size() >= kMaxQueuedJobs) {
      queue.pop_front();
    }
    queue.push_back(std::move(job));
  }

  autoplayId = id;
  queueCv.notify_one();
  return id;
}

SpeechQueue::ClipState SpeechQueue::StateOf(SpeechClipId id) const {
  if (id == kNoSpeechClip) {
    return ClipState::Unavailable;
  }
  const auto it = clips.find(id);
  if (it == clips.end()) {
    return ClipState::Unavailable;
  }

  const Clip &clip = it->second;
  if (clip.failed) {
    return ClipState::Failed;
  }
  if (clip.dropped) {
    return ClipState::Dropped;
  }
  if (clip.wav.empty()) {
    return ClipState::Preparing;
  }
  if (activeId == id && IsMusicValid(music)) {
    if (IsMusicStreamPlaying(music)) {
      return ClipState::Playing;
    }
    if (paused) {
      return ClipState::Paused;
    }
  }
  return ClipState::Ready;
}

float SpeechQueue::Duration(SpeechClipId id) const {
  const auto it = clips.find(id);
  if (it == clips.end() || it->second.wav.size() <= 44 || sampleRate <= 0) {
    return 0.0f;
  }
  // Derived from the retained bytes rather than from the stream, so a clip that has
  // never been played still reports its length and the scrubber has a range.
  const size_t dataBytes = it->second.wav.size() - 44;
  return static_cast<float>(dataBytes) / 2.0f / static_cast<float>(sampleRate);
}

float SpeechQueue::Position(SpeechClipId id) const {
  if (id == kNoSpeechClip || activeId != id || !IsMusicValid(music)) {
    return 0.0f;
  }
  return GetMusicTimePlayed(music);
}

void SpeechQueue::EnsureActive(SpeechClipId id) {
  if (activeId == id && IsMusicValid(music)) {
    return;
  }
  UnloadMusic();

  const auto it = clips.find(id);
  if (it == clips.end() || it->second.wav.empty()) {
    return;
  }

  Clip &clip = it->second;
  music = LoadMusicStreamFromMemory(".wav", clip.wav.data(),
                                    static_cast<int>(clip.wav.size()));
  if (!IsMusicValid(music)) {
    music = Music{};
    return;
  }

  activeId = id;
  paused = false;
  SetMusicVolume(music, volume);
}

void SpeechQueue::UnloadMusic() {
  if (IsMusicValid(music)) {
    StopMusicStream(music);
    UnloadMusicStream(music);
  }
  music = Music{};
  activeId = kNoSpeechClip;
  paused = false;
}

void SpeechQueue::Toggle(SpeechClipId id) {
  if (id == kNoSpeechClip || muted.load()) {
    return;
  }
  const auto it = clips.find(id);
  if (it == clips.end() || it->second.wav.empty()) {
    return;
  }

  const bool wasThisClip = (activeId == id) && IsMusicValid(music);
  EnsureActive(id);
  if (activeId != id || !IsMusicValid(music)) {
    return;
  }

  if (wasThisClip && IsMusicStreamPlaying(music)) {
    PauseMusicStream(music);
    paused = true;
  } else if (wasThisClip && paused) {
    ResumeMusicStream(music);
    paused = false;
  } else {
    // Never started, or finished, or stopped: begin from the top. StopMusicStream
    // already rewound the decoder, but a clip that ran to the end needs the seek.
    SeekMusicStream(music, 0.0f);
    PlayMusicStream(music);
    paused = false;
  }
}

void SpeechQueue::Seek(SpeechClipId id, float seconds) {
  if (id == kNoSpeechClip) {
    return;
  }
  const auto it = clips.find(id);
  if (it == clips.end() || it->second.wav.empty()) {
    return;
  }

  // Scrubbing a clip that is not loaded loads it paused at that point, so the
  // scrubber works before the first play. Deliberate: dragging a seek bar and
  // having nothing happen is worse than a clip that is loaded but silent.
  EnsureActive(id);
  if (activeId != id || !IsMusicValid(music)) {
    return;
  }

  SeekMusicStream(music, std::clamp(seconds, 0.0f, Duration(id)));
}

void SpeechQueue::StopClip(SpeechClipId id) {
  if (id == kNoSpeechClip || activeId != id || !IsMusicValid(music)) {
    return;
  }
  // Resets the decoder to the first frame, so the next play starts from the top
  // while the position readout goes back to zero.
  StopMusicStream(music);
  paused = false;
}

void SpeechQueue::Stop() {
  autoplayId = kNoSpeechClip;
  stopRequested = true;
  queueCv.notify_all();

  {
    std::lock_guard<std::mutex> lock(queueMutex);
    queue.clear();
  }
  {
    std::lock_guard<std::mutex> lock(publishMutex);
    published.clear();
  }

  // Unblocks the worker if it is inside Speak(). Called from the main thread, so it
  // must not wait for the worker -- and it does not.
  if (backend != nullptr) {
    backend->Stop();
  }

  if (IsMusicValid(music)) {
    StopMusicStream(music);
  }
  paused = false;
}

void SpeechQueue::SetMuted(bool value) {
  muted = value;
  if (value) {
    Stop();
  }
}

bool SpeechQueue::IsSpeaking() const {
  if (IsMusicValid(music) && IsMusicStreamPlaying(music)) {
    return true;
  }
  // Queued or mid-synthesis counts as "about to be audible", so the status line
  // does not flicker to idle during the ~2 s a reading takes to prepare.
  for (const auto &entry : clips) {
    const Clip &clip = entry.second;
    if (clip.wav.empty() && !clip.failed && !clip.dropped) {
      return true;
    }
  }
  return false;
}

void SpeechQueue::EvictIfNeeded() {
  size_t retained = 0;
  for (const auto &entry : clips) {
    const Clip &clip = entry.second;
    if (!clip.wav.empty() && !clip.dropped) {
      ++retained;
    }
  }

  while (retained > kMaxRetainedClips) {
    // The oldest that is not the one currently loaded, so eviction can never pull
    // the bytes out from under a live Music stream. Dropping `wav` while dr_wav
    // still points into it would be a use-after-free, which is exactly why the
    // active clip is skipped rather than handled.
    SpeechClipId victim = kNoSpeechClip;
    for (const auto &entry : clips) {
      const Clip &clip = entry.second;
      if (clip.wav.empty() || clip.dropped || entry.first == activeId) {
        continue;
      }
      if (victim == kNoSpeechClip || entry.first < victim) {
        victim = entry.first;
      }
    }
    if (victim == kNoSpeechClip) {
      break;  // Only the active clip is left; nothing safe to drop.
    }

    Clip &clip = clips[victim];
    clip.wav.clear();
    clip.wav.shrink_to_fit();
    clip.dropped = true;
    --retained;
  }
}

void SpeechQueue::Update() {
  if (!ready) {
    return;
  }

  // 1. Install whatever the worker finished since the last frame.
  std::vector<Published> incoming;
  {
    std::lock_guard<std::mutex> lock(publishMutex);
    incoming.swap(published);
  }

  for (Published &result : incoming) {
    const auto it = clips.find(result.id);
    if (it == clips.end()) {
      continue;
    }
    Clip &clip = it->second;

    if (result.samples.empty()) {
      clip.failed = true;
      clip.error = result.error.empty() ? "the speech engine produced no audio."
                                       : result.error;
      continue;
    }

    clip.wav = MakeWav(result.samples, sampleRate);
    // `result.samples` dies here; the WAV now owns the PCM.

    if (autoplayId == result.id) {
      autoplayId = kNoSpeechClip;
      const bool somethingPlaying = IsMusicValid(music) && IsMusicStreamPlaying(music);
      if (!muted.load() && !somethingPlaying) {
        EnsureActive(result.id);
        if (activeId == result.id && IsMusicValid(music)) {
          SeekMusicStream(music, 0.0f);
          PlayMusicStream(music);
          paused = false;
        }
      }
    }
  }

  if (!incoming.empty()) {
    EvictIfNeeded();
  }

  // 2. Keep the active stream fed. raylib requires this every frame or a Music
  //    stream stalls after its first buffers drain.
  if (IsMusicValid(music)) {
    UpdateMusicStream(music);
  }
}

void SpeechQueue::Worker() {
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(queueMutex);
      queueCv.wait(lock, [this] {
        return quitting.load() || stopRequested.load() || !queue.empty();
      });

      if (quitting.load()) {
        return;
      }
      if (stopRequested.load()) {
        // Consume the stop and go back to waiting. Nothing queued outlives it,
        // because Stop() empties the queue under the same lock.
        stopRequested = false;
        continue;
      }
      if (queue.empty()) {
        continue;
      }

      job = std::move(queue.front());
      queue.pop_front();
    }

    Published result;
    result.id = job.id;

    if (backend != nullptr) {
      backend->Speak(job.text, [&result](const int16_t *samples, size_t count) {
        result.samples.insert(result.samples.end(), samples, samples + count);
      });
      if (result.samples.empty()) {
        result.failed = true;
        result.error = "the speech engine produced no audio.";
      }
    } else {
      result.failed = true;
      result.error = "no speech engine is configured.";
    }

    {
      std::lock_guard<std::mutex> lock(publishMutex);
      published.push_back(std::move(result));
    }
  }
}

std::string SpeechQueue::StatusLine() const {
  if (backend == nullptr) {
    return {};
  }
  return backend->getEngineName() + " / " + backend->getVoiceName() + " @ " +
         std::to_string(backend->SampleRate()) + " Hz";
}
