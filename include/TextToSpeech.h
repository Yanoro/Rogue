#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

// A local speech engine, abstracted the same way AI.h abstracts a chat backend:
// the game talks to this, and which engine actually speaks is a settings choice.
//
// Deliberately narrow. The only things every engine has in common are "text in,
// int16 mono PCM out" and the sample rate -- and the caller genuinely needs the
// rate, because the audio buffer it builds has to be created at that rate, and a
// mismatch is audible as the wrong pitch rather than as an error. Piper medium
// voices are 22050 Hz; Kokoro is 24000 Hz.
class TextToSpeech {
public:
  // Called once per chunk of synthesised audio, on the synthesis thread.
  //
  // The samples are borrowed: the engine may reuse or free them as soon as the
  // callback returns, so a caller that wants to keep them must copy.
  using AudioCallback = std::function<void(const int16_t *samples, size_t count)>;

  // Synthesises `text`, calling `onAudio` for each chunk as it is produced.
  //
  // BLOCKING: runs for as long as the synthesis takes, so callers hand it to a
  // worker thread and never call it from the frame loop.
  //
  // Returns false when the engine could not be started or produced no audio at
  // all. An utterance that was cut short by Stop() still returns true: the audio
  // that did arrive was already handed to the callback.
  virtual bool Speak(const std::string &text, const AudioCallback &onAudio) = 0;

  // Asks an in-flight Speak to give up, and returns without waiting for it.
  //
  // Called from whichever thread wants the sound to stop -- in practice the main
  // thread -- so an implementation must not assume it runs on the synthesis
  // thread. The synthesis thread is expected to notice, return promptly, and
  // treat the utterance as finished.
  virtual void Stop() = 0;

  // The rate audio must be played back at.
  virtual int SampleRate() const = 0;

  // For the window's status line and the log: which engine is speaking, and with
  // which voice.
  virtual std::string getEngineName() const = 0;
  virtual std::string getVoiceName() const = 0;

  virtual ~TextToSpeech() = default;
};
