#include "PiperSpeech.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include <nlohmann/json.hpp>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

// Piper has no length limit of its own, but the text goes through a pipe we write
// before we start reading, so an utterance larger than the pipe buffer could
// deadlock against the child writing its output. A few kilobytes is far more than
// any spoken line needs; the cap exists so a runaway caller cannot wedge the game.
constexpr size_t kMaxTextBytes = 8192;

// Read size. The child writes int16 mono, and this is the number of samples per
// read, so each read moves 8 KB.
constexpr size_t kMaxSamplesPerRead = 4096;

// The voice's rate, read out of the model's config JSON. Piper resolves the config
// as "<model>.onnx.json", which is what the download puts next to the model.
bool ReadSampleRate(const std::string &configPath, int &out) {
  std::ifstream file(configPath);
  if (!file.is_open()) {
    return false;
  }

  nlohmann::json config;
  try {
    config = nlohmann::json::parse(file);
  } catch (const std::exception &) {
    return false;
  }

  if (!config.contains("audio") || !config["audio"].is_object()) {
    return false;
  }
  const nlohmann::json &audio = config["audio"];
  if (!audio.contains("sample_rate") || !audio["sample_rate"].is_number_integer()) {
    return false;
  }

  const int rate = audio["sample_rate"].get<int>();
  if (rate <= 0) {
    return false;
  }

  out = rate;
  return true;
}

std::string StemOf(const std::string &path) {
  return std::filesystem::path(path).stem().string();
}

// Resolves `binary` to a real path, looking it up on PATH when it is a bare name.
//
// Done here rather than left to execvp because execvp's failure is invisible from
// the parent: the child dies with 127, the pipe closes, and a closed pipe is
// indistinguishable from "piper had nothing to say". Catching it at startup turns
// a silent no-speech run into a message naming the problem and the fix.
std::string ResolveOnPath(const std::string &binary) {
  if (binary.find('/') != std::string::npos) {
    return ::access(binary.c_str(), X_OK) == 0 ? binary : std::string();
  }

  const char *path = std::getenv("PATH");
  if (path == nullptr) {
    return {};
  }

  std::stringstream entries(path);
  std::string dir;
  while (std::getline(entries, dir, ':')) {
    if (dir.empty()) {
      continue;
    }
    const std::filesystem::path candidate = std::filesystem::path(dir) / binary;
    if (::access(candidate.c_str(), X_OK) == 0) {
      // Absolute, so the reported path is the one that will actually run.
      std::error_code ec;
      const std::filesystem::path absolute =
          std::filesystem::absolute(candidate, ec);
      return ec ? candidate.string() : absolute.string();
    }
  }
  return {};
}

// Writes everything, tolerating short writes and signals. Returns false on a real
// error. `bytes` is capped by the caller, so this cannot block on a full pipe.
bool WriteAll(int fd, const char *data, size_t bytes) {
  size_t written = 0;
  while (written < bytes) {
    const ssize_t n = write(fd, data + written, bytes - written);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    written += static_cast<size_t>(n);
  }
  return true;
}

}  // namespace

std::unique_ptr<PiperSpeech> PiperSpeech::Create(const std::string &binary,
                                                const std::string &voicePath,
                                                std::string &error) {
  if (binary.empty()) {
    error = "no piper binary is configured.";
    return nullptr;
  }
  if (voicePath.empty()) {
    error = "no voice model is configured.";
    return nullptr;
  }

  const std::string resolvedBinary = ResolveOnPath(binary);
  if (resolvedBinary.empty()) {
    error = "the piper binary \"" + binary +
            "\" was not found on PATH. Run the game from nix-shell "
            "(./compile_and_run.sh), or set designer.tts.binary to an absolute "
            "path such as /nix/store/<hash>-piper-tts-1.3.0/bin/piper.";
    return nullptr;
  }

  if (!std::filesystem::exists(voicePath)) {
    error = "the voice model does not exist: " + voicePath +
            " (see SETTINGS.md for the one-time download).";
    return nullptr;
  }

  const std::string configPath = voicePath + ".json";
  int sampleRate = 0;
  if (!ReadSampleRate(configPath, sampleRate)) {
    // Not fatal: a voice without a readable config is far more likely to be a
    // truncated download than a genuinely different rate, and 22050 Hz is what
    // every piper medium voice uses. Say so rather than refusing to speak.
    error = "could not read the sample rate from " + configPath +
            "; assuming 22050 Hz.";
    sampleRate = 22050;
  } else {
    error.clear();
  }

  std::unique_ptr<PiperSpeech> speech(new PiperSpeech());
  speech->binary = resolvedBinary;
  speech->voicePath = voicePath;
  speech->voiceName = StemOf(voicePath);
  speech->sampleRate = sampleRate;
  return speech;
}

bool PiperSpeech::Speak(const std::string &text, const AudioCallback &onAudio) {
  stopRequested = false;

  std::string prompt = text;
  if (prompt.size() > kMaxTextBytes) {
    prompt.resize(kMaxTextBytes);
  }
  if (prompt.empty()) {
    return false;
  }
  // The CLI synthesises one line per line of input, so the text has to arrive as
  // exactly one line. Newlines inside it would be treated as separate utterances.
  for (char &c : prompt) {
    if (c == '\n' || c == '\r') {
      c = ' ';
    }
  }

  int toChild[2] = {-1, -1};   // parent writes, child reads (text in)
  int fromChild[2] = {-1, -1}; // child writes, parent reads (PCM out)
  if (pipe(toChild) != 0 || pipe(fromChild) != 0) {
    if (toChild[0] >= 0) {
      close(toChild[0]);
      close(toChild[1]);
    }
    return false;
  }

  const pid_t pid = fork();
  if (pid < 0) {
    close(toChild[0]);
    close(toChild[1]);
    close(fromChild[0]);
    close(fromChild[1]);
    return false;
  }

  if (pid == 0) {
    // Child. Only async-signal-safe calls from here to execvp.
    dup2(toChild[0], STDIN_FILENO);
    dup2(fromChild[1], STDOUT_FILENO);
    close(toChild[0]);
    close(toChild[1]);
    close(fromChild[0]);
    close(fromChild[1]);

    // stderr is deliberately left inherited: piper's own "Loading voice" and
    // warning lines are the fastest way to see why an utterance came back empty.
    execlp(binary.c_str(), "piper", "-m", voicePath.c_str(), "--output-raw",
           static_cast<char *>(nullptr));
    _exit(127);
  }

  // Parent.
  close(toChild[0]);
  close(fromChild[1]);

  {
    std::lock_guard<std::mutex> lock(childMutex);
    childPid = static_cast<int>(pid);
  }

  // Write the whole utterance, then close stdin. The close is what tells the
  // child there is nothing more coming, so it synthesises this one line and exits.
  WriteAll(toChild[1], prompt.data(), prompt.size());
  WriteAll(toChild[1], "\n", 1);
  close(toChild[1]);

  std::vector<int16_t> samples(kMaxSamplesPerRead);
  std::vector<uint8_t> raw(kMaxSamplesPerRead * 2 + 1);
  // A read can end mid-sample. The odd byte is carried into the next read rather
  // than dropped, so the stream is never left half a sample out of alignment.
  size_t carry = 0;
  bool producedAnything = false;

  for (;;) {
    const ssize_t n =
        read(fromChild[0], raw.data() + carry, kMaxSamplesPerRead * 2 - carry);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (n == 0) {
      break;
    }

    const size_t total = carry + static_cast<size_t>(n);
    const size_t whole = total / 2;
    if (whole > 0) {
      std::memcpy(samples.data(), raw.data(), whole * sizeof(int16_t));
      onAudio(samples.data(), whole);
      producedAnything = true;
    }

    carry = total - whole * 2;
    if (carry == 1) {
      raw[0] = raw[total - 1];
    }

    if (stopRequested.load()) {
      break;
    }
  }

  close(fromChild[0]);

  // Always reap, including on the stop path: a killed child that is never waited
  // on stays a zombie for the rest of the session, one per interrupted utterance.
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }

  {
    std::lock_guard<std::mutex> lock(childMutex);
    childPid = -1;
  }

  return producedAnything;
}

void PiperSpeech::Stop() {
  stopRequested = true;

  std::lock_guard<std::mutex> lock(childMutex);
  if (childPid > 0) {
    // SIGTERM, not SIGKILL: piper is Python, and the default handler exits it
    // cleanly. Speak's waitpid collects it either way.
    kill(childPid, SIGTERM);
  }
}
