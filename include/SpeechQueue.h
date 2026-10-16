#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <raylib.h>

#include "TextToSpeech.h"

// A handle to one synthesised utterance. Returned by SpeechQueue::Enqueue and kept
// by whoever wants a transport bar for it; kNoSpeechClip means "there is no audio
// for this, and there never was".
using SpeechClipId = uint64_t;
inline constexpr SpeechClipId kNoSpeechClip = 0;

// Owns everything that turns "speak this" into sound, for every utterance the game
// has spoken this session, each addressable on its own.
//
// The thread split is the point, and it mirrors the AI path's:
//
//   - Enqueue() and the transport calls are made from the main thread, by whichever
//     window decided something should be said or wants to scrub it.
//   - The worker runs the blocking TextToSpeech::Speak, which shells out and waits,
//     and hands the collected PCM back through a mutex-guarded list.
//   - Update() runs on the main thread once per frame, is the only place raylib's
//     audio state is touched, and must NOT be called from a window's Draw() --
//     Draw() only runs while that window is visible, so a pane closed to watch the
//     world would silence playback mid-sentence.
//
// WHY A WHOLE UTTERANCE AND NOT A STREAM
// --------------------------------------
// Feeding raylib an AudioStream chunk by chunk is a trap in 5.5: UpdateAudioStream
// writes into one half of a double buffer and ZERO-FILLS the rest of that half, so
// any write smaller than the sub-buffer inserts a burst of silence -- and the
// sub-buffer size is not queryable. An utterance is therefore collected whole.
//
// WHY MUSIC AND NOT SOUND
// -----------------------
// raylib's Sound has no seek and no time API at all, so a scrub bar is impossible
// with it. A Music stream has the whole transport (SeekMusicStream,
// GetMusicTimePlayed, GetMusicTimeLength, Pause/Resume/Stop), and it decodes on
// demand instead of expanding everything to the device format up front. The
// utterance is wrapped in a 44-byte WAV header in memory and loaded with
// LoadMusicStreamFromMemory.
//
// Only ONE clip has a live Music stream at a time. raylib's audio buffer pool holds
// 16 entries shared with Sound objects, so keeping a stream open per retained reply
// would exhaust it; the retained WAV bytes are cheap (~1-2 MB each) and the stream
// is not.
class SpeechQueue {
public:
  enum class ClipState {
    Unavailable,  // never queued (speech was off or muted), or an id we do not know
    Preparing,    // queued, synthesis still in flight
    Ready,        // audio in hand, not currently playing
    Playing,
    Paused,
    Failed,       // the engine was asked and produced nothing
    Dropped,      // had audio, evicted by the retention cap
  };

  // How many utterances keep their audio. Past this the oldest that is not playing
  // is evicted, and its bar offers to speak it again instead.
  static constexpr size_t kMaxRetainedClips = 24;

  // `backend` is borrowed -- Game owns it for the whole run -- and may be null,
  // which is a supported state meaning "nothing can speak". `volume` is the
  // playback gain, 1.0 being unattenuated.
  SpeechQueue(TextToSpeech *backend, float volume);
  ~SpeechQueue();

  SpeechQueue(const SpeechQueue &) = delete;
  SpeechQueue &operator=(const SpeechQueue &) = delete;

  // Records whether the audio device is usable and starts the worker. False means
  // Enqueue returns kNoSpeechClip and nothing is ever heard.
  bool Init();
  // Joins the worker and releases the stream. Must run before CloseAudioDevice.
  void Shutdown();

  // Queues one utterance and returns its handle, or kNoSpeechClip when there is no
  // engine, the device is unusable, or speech is muted. The caller keeps the id to
  // render a bar for it.
  SpeechClipId Enqueue(std::string text);

  // Transport. All main thread; all no-ops for an unknown or audioless id.
  ClipState StateOf(SpeechClipId id) const;
  // Seconds; 0 while preparing, and derived from the retained bytes rather than
  // from the stream, so it answers even before the clip is ever played.
  float Duration(SpeechClipId id) const;
  float Position(SpeechClipId id) const;

  // Play / pause / restart, depending on where the clip is.
  void Toggle(SpeechClipId id);
  // Scrubbing a clip that has never been played loads it, paused, at that point.
  void Seek(SpeechClipId id, float seconds);
  // Stops and rewinds to the start, keeping the audio for another play.
  void StopClip(SpeechClipId id);

  // Stops playback, discards anything queued or being synthesised, and drops the
  // pending autoplay. Retained clips are deliberately KEPT: stopping a reading is
  // not the same as deciding you never want to hear it, and the bars stay usable.
  void Stop();

  // Muting silences what is playing and cancels what is queued. A mute button that
  // leaves forty seconds of speech running is not a mute button.
  void SetMuted(bool value);
  bool IsMuted() const { return muted.load(); }

  // True while something is audible or about to be.
  bool IsSpeaking() const;
  bool HasBackend() const { return backend != nullptr; }
  bool IsReady() const { return ready; }

  // Non-empty when the device claims to be ready but no real backend library could
  // be loaded, which means miniaudio is on its null device and every sample is
  // thrown away. raylib reports success in that state, so this is the only signal
  // that sound is not actually going anywhere.
  const std::string &AudioWarning() const { return audioWarning; }

  // "piper / en_GB-alba-medium @ 22050 Hz", or empty with no backend.
  std::string StatusLine() const;

  // Feeds the active stream. Main thread, once per frame.
  void Update();

private:
  void Worker();
  // Makes `id` the clip with a live Music stream, unloading whatever held it.
  void EnsureActive(SpeechClipId id);
  void UnloadMusic();
  // Drops the oldest non-playing audio once more than kMaxRetainedClips are kept.
  void EvictIfNeeded();

  struct Clip {
    SpeechClipId id = 0;
    // The utterance as a complete in-memory WAV. dr_wav keeps a POINTER into this
    // buffer rather than copying it, so it must outlive the Music stream loaded
    // from it -- which is why the bytes are freed only after the stream is gone.
    std::vector<uint8_t> wav;
    bool failed = false;
    std::string error;
    bool dropped = false;
  };

  // The worker's handoff: written by the worker, drained by Update().
  struct Published {
    SpeechClipId id = 0;
    std::vector<int16_t> samples;
    bool failed = false;
    std::string error;
  };

  struct Job {
    SpeechClipId id = 0;
    std::string text;
  };

  TextToSpeech *backend = nullptr;
  float volume = 1.0f;
  int sampleRate = 22050;
  bool ready = false;
  std::string audioWarning;

  std::thread worker;
  // Asks the worker to leave its loop for good (destruction only).
  std::atomic<bool> quitting{false};
  // Asks the worker to abandon the current utterance and anything queued.
  std::atomic<bool> stopRequested{false};
  std::atomic<bool> muted{false};

  std::mutex queueMutex;
  std::condition_variable queueCv;
  std::deque<Job> queue;

  std::mutex publishMutex;
  std::vector<Published> published;

  // ---- main thread only from here ----
  std::unordered_map<SpeechClipId, Clip> clips;
  SpeechClipId nextId = 1;
  SpeechClipId activeId = kNoSpeechClip;
  Music music{};
  bool paused = false;
  // The clip that should start playing when its synthesis lands. Cleared by Stop()
  // and by muting, so an utterance the user cancelled cannot start talking later.
  SpeechClipId autoplayId = kNoSpeechClip;
};
