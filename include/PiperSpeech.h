#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "TextToSpeech.h"

// Piper (the `piper-tts` package, piper1-gpl) driven as a **subprocess**.
//
// Out of process on purpose, for two reasons that both bite if it is in-process:
//
//   1. AddressSanitizer. AIRogue is built with ASan, so linking libpiper would put
//      a system-built onnxruntime and espeak-ng inside the sanitizer's address
//      space -- leak reports at every exit and interceptor noise on the paths
//      that matter. A child process is invisible to all of it, and a wedged
//      synthesis is a kill() instead of a hung game.
//   2. Licensing. libpiper is GPL-3.0. Linking it in-process would put the
//      combined work under the GPL; running the CLI is just using a program.
//
// ONE PROCESS PER UTTERANCE, which is what makes the pipe self-framing.
//
// Piper's CLI reads text lines from stdin and writes raw PCM to stdout, so a
// long-lived process is tempting. It cannot be used that way: `--output-raw`
// carries no end-of-utterance marker, so a reader cannot tell where one
// utterance's samples stop and the next one's begin -- it does not know the
// sample count in advance. Letting the process exit close the pipe gives correct
// framing for free. The reply takes seconds to generate and the model load is
// under a second, so paying it per utterance is not on the critical path.
class PiperSpeech : public TextToSpeech {
public:
  // Resolves the voice's sample rate from its config JSON up front, because the
  // caller has to build its audio buffer at the right rate before anything is
  // spoken.
  //
  // Returns nullptr and fills `error` when the binary or the voice cannot be
  // used. That is a supported state, exactly like a null designer backend: the
  // window reports it rather than the game crashing on a missing model file.
  //
  // `binary` is either an absolute path or a name resolved on PATH -- "piper" by
  // default, which is what the nix-shell environment provides.
  static std::unique_ptr<PiperSpeech> Create(const std::string &binary,
                                             const std::string &voicePath,
                                             std::string &error);

  bool Speak(const std::string &text, const AudioCallback &onAudio) override;
  void Stop() override;

  int SampleRate() const override { return sampleRate; }
  std::string getEngineName() const override { return "piper"; }
  std::string getVoiceName() const override { return voiceName; }

private:
  PiperSpeech() = default;

  std::string binary;
  std::string voicePath;
  // The model file's stem, e.g. "en_GB-alba-medium", for display.
  std::string voiceName;
  int sampleRate = 22050;

  // The live child, so Stop() can kill it. Written by the synthesis thread,
  // read by whichever thread calls Stop().
  std::mutex childMutex;
  int childPid = -1;

  // Set by Stop(), cleared at the start of every Speak. The child being killed is
  // what actually unblocks the read loop; this only makes the intent explicit and
  // stops a Speak that has not spawned yet.
  std::atomic<bool> stopRequested{false};
};
