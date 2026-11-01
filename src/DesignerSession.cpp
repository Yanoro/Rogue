#include "DesignerSession.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>

#include "DebugLog.h"
#include "Game.h"
#include "LogPaths.h"
#include "Settings.h"
#include "TimeUtils.hpp"
#include "imgui.h"

namespace {

// How many of the most recent messages are replayed to the model. Unbounded history is
// the one cost that grows without limit in a chat like this, and a design is a large
// object: after a handful of them every request would carry every blueprint ever made.
// The cap is generous enough that "change the door position" still has the previous
// design in view.
constexpr size_t kMaxHistoryMessages = 8;

// The sentence a failed transport produces. `label` is capitalised for debug messages, so
// the first letter is lowered rather than carrying a second spelling of the same window
// name through the constructor.
std::string LowerFirst(std::string text) {
  if (!text.empty() && text.front() >= 'A' && text.front() <= 'Z') {
    text.front() = static_cast<char>(text.front() - 'A' + 'a');
  }
  return text;
}

const std::string kEmptyString;

}  // namespace

// ---------------------------------------------------------------------------
// Reply text
// ---------------------------------------------------------------------------

namespace DesignerReply {

std::string StripReasoning(const std::string &text) {
  static constexpr const char *kOpen = "<think>";
  static constexpr const char *kClose = "</think>";
  static constexpr size_t kOpenLength = 7;
  static constexpr size_t kCloseLength = 8;

  std::string out;
  out.reserve(text.size());

  size_t i = 0;
  while (i < text.size()) {
    if (text.compare(i, kOpenLength, kOpen) == 0) {
      const size_t close = text.find(kClose, i + kOpenLength);
      if (close == std::string::npos) {
        break;  // an unterminated block swallows the rest
      }
      i = close + kCloseLength;
      continue;
    }
    out += text[i++];
  }
  return out;
}

std::string HideJsonBlock(const std::string &text, const std::string &marker) {
  const size_t fence = text.find(kJsonFence);
  if (fence == std::string::npos) {
    return text;
  }
  const size_t bodyStart = text.find('\n', fence);
  if (bodyStart == std::string::npos) {
    return text;
  }

  std::string out = text.substr(0, fence);
  const size_t close = text.find("```", bodyStart);
  out += close == std::string::npos ? marker : marker + text.substr(close + 3);

  // Collapse the blank line the removed block leaves behind, and trim the end, so the
  // transcript does not grow a gap between every reply.
  while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) {
    out.pop_back();
  }
  std::string collapsed;
  for (size_t i = 0; i < out.size(); ++i) {
    collapsed += out[i];
    if (out[i] == '\n') {
      while (i + 1 < out.size() && (out[i + 1] == '\n' || out[i + 1] == ' ')) {
        ++i;
      }
    }
  }
  return collapsed;
}

std::string ExtractJsonBlock(const std::string &text) {
  const size_t fence = text.find(kJsonFence);
  if (fence == std::string::npos) {
    return {};
  }

  const size_t bodyStart = text.find('\n', fence);
  if (bodyStart == std::string::npos) {
    return {};
  }

  const size_t close = text.find("```", bodyStart);
  if (close == std::string::npos) {
    // An unterminated fence: take the rest. A model that forgot to close it has usually
    // still produced the object, and reporting "no JSON block" would be a less useful
    // error than whatever the parser says about the text.
    return text.substr(bodyStart + 1);
  }
  return text.substr(bodyStart + 1, close - bodyStart - 1);
}

std::string ProseBeforeJson(const std::string &text) {
  const size_t fence = text.find(kJsonFence);
  const std::string prose = fence == std::string::npos ? text : text.substr(0, fence);

  // Trimmed because the fence sits on its own line, so the prose otherwise ends with the
  // blank line that separated them -- which the engine would read as a pause at the end
  // of every utterance.
  const size_t first = prose.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return {};
  }
  const size_t last = prose.find_last_not_of(" \t\r\n");
  return prose.substr(first, last - first + 1);
}

std::string DisplayAnswer(const std::string &answer, bool showThoughts, bool showJson,
                          const std::string &marker) {
  const std::string text = showThoughts ? answer : StripReasoning(answer);
  return showJson ? text : HideJsonBlock(text, marker);
}

}  // namespace DesignerReply

// ---------------------------------------------------------------------------
// One exchange
// ---------------------------------------------------------------------------

std::string DesignerSession::Exchange::ParsedAnswer() const {
  return DesignerReply::StripReasoning(answer);
}

std::string DesignerSession::Exchange::HistoryAnswer(
    const std::string &hiddenMarker) const {
  return DesignerReply::HideJsonBlock(answer, hiddenMarker);
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

DesignerSession::DesignerSession(Game *game, AI *backend, std::string modelLabel,
                                 std::string logDirectory, std::string logSuffix,
                                 std::string label, std::string hiddenMarker)
    : game(game),
      backend(backend),
      modelLabel(std::move(modelLabel)),
      logDirectory(std::move(logDirectory)),
      logSuffix(std::move(logSuffix)),
      label(std::move(label)),
      hiddenMarker(std::move(hiddenMarker)) {}

// ---------------------------------------------------------------------------
// Conversations
// ---------------------------------------------------------------------------

bool DesignerSession::ValidConversation(int index) const {
  return index >= 0 && index < static_cast<int>(conversations.size());
}

int DesignerSession::AddConversation(std::string contextId,
                                     std::string systemPrompt) {
  Conversation conversation;
  conversation.contextId = std::move(contextId);
  conversation.systemPrompt = std::move(systemPrompt);
  conversations.push_back(std::move(conversation));
  return static_cast<int>(conversations.size()) - 1;
}

void DesignerSession::SetSystemPrompt(int conversation, std::string prompt) {
  if (!ValidConversation(conversation)) return;
  conversations[conversation].systemPrompt = std::move(prompt);
}

const std::string &DesignerSession::SystemPrompt(int conversation) const {
  if (!ValidConversation(conversation)) return kEmptyString;
  return conversations[conversation].systemPrompt;
}

const std::string &DesignerSession::ContextId(int conversation) const {
  if (!ValidConversation(conversation)) return kEmptyString;
  return conversations[conversation].contextId;
}

// ---------------------------------------------------------------------------
// Requests
// ---------------------------------------------------------------------------

bool DesignerSession::Send(const std::string &prompt, int conversation, bool fromUser,
                           bool wasSet) {
  if (backend == nullptr || prompt.empty() || IsBusy() || !ValidConversation(conversation)) {
    return false;
  }

  // Snapshot the conversation on the main thread. The worker must not read a history the
  // main thread is about to mutate, and the request it is answering has to be built from
  // the conversation as it stood when the request was made.
  const Conversation &active = conversations[conversation];
  const std::string context = active.contextId;

  std::vector<ChatMessage> request;
  if (!active.systemPrompt.empty()) {
    request.push_back({"system", active.systemPrompt});
  }
  const size_t firstKept = active.history.size() > kMaxHistoryMessages
                               ? active.history.size() - kMaxHistoryMessages
                               : 0;
  for (size_t i = firstKept; i < active.history.size(); ++i) {
    request.push_back(active.history[i]);
  }
  request.push_back({"user", prompt});

  Exchange exchange;
  exchange.conversation = conversation;
  exchange.prompt = prompt;
  exchange.fromUser = fromUser;
  exchange.wasSet = wasSet;

  size_t index = 0;
  {
    std::lock_guard<std::mutex> lock(transcriptMutex);
    exchanges.push_back(std::move(exchange));
    index = exchanges.size() - 1;
  }

  NoteSend(request);

  // The worker keeps the game loop responsive for providers whose generateStream blocks.
  // OllamaAI's does not -- it returns immediately and streams from its own thread -- which
  // is why completion is signalled by the empty-token sentinel and never by this function
  // returning.
  //
  // Nothing is written to the transcript from here: the stream callback runs on a provider
  // thread and fires once per token, so logging there would mean a file open per token.
  const std::string failurePrefix = "The " + LowerFirst(label) + " backend could not accept the request.";
  AI *worker = backend;
  std::thread([this, index, request = std::move(request), worker, context,
               failurePrefix]() {
    // A fresh provider-side context per request, because the history is sent explicitly:
    // leaving the previous request's tokens in place would replay the conversation twice.
    worker->resetContext(context);

    std::string failure;
    const bool delivered = worker->generateStream(
        context, request,
        [this, index](const std::string &token) {
          std::lock_guard<std::mutex> lock(transcriptMutex);
          if (index >= exchanges.size()) {
            return;
          }
          Exchange &exchange = exchanges[index];
          if (!token.empty()) {
            // Stored verbatim. Reasoning is stripped when the exchange is inspected, and
            // again at display time, so the checkbox can put it back -- which a filter
            // applied here could not.
            exchange.answer += token;
            return;
          }
          // Empty token: every provider uses it as the end-of-stream sentinel.
          exchange.done = true;
          if (exchange.answer.empty()) {
            exchange.failed = true;
            exchange.error =
                "No answer came back. Check that the backend is running and "
                "that the model is available.";
          }
        });
    // generateStream's remaining parameters (a stop token and an image list) have
    // defaults, and a designer is text-only, so neither is passed.

    if (!delivered) {
      failure = failurePrefix;
    }

    // NOTE the absence of a "stream finished" branch here. OllamaAI's generateStream
    // dispatches its own thread and returns immediately, so returning means only "the
    // request was accepted". Treating the return as completion marks the exchange done
    // while the answer is still empty, which raises the bogus "stream ended without a
    // reply" error, inspects a half-received reply, and -- worst -- starts a retry that
    // calls resetContext on a context whose first stream is still in flight.
    if (!failure.empty()) {
      std::lock_guard<std::mutex> lock(transcriptMutex);
      if (index < exchanges.size()) {
        Exchange &exchange = exchanges[index];
        exchange.done = true;
        exchange.failed = true;
        exchange.error = failure;
      }
    }
  }).detach();

  return true;
}

void DesignerSession::NoteSend(const std::vector<ChatMessage> &request) {
  // What actually goes on the wire, recorded before the thread is started so the log and
  // the request cannot disagree: the exact message list, including how much of the
  // history survived the cap. The system prompt is written out once by the window's
  // session header, so here it is recorded by size -- repeating a 3KB prompt on every
  // request would bury the conversation it is meant to illuminate.
  size_t totalChars = 0;
  size_t systemCount = 0;
  size_t userCount = 0;
  size_t assistantCount = 0;
  for (const ChatMessage &message : request) {
    totalChars += message.content.size();
    if (message.role == "system") ++systemCount;
    else if (message.role == "user") ++userCount;
    else if (message.role == "assistant") ++assistantCount;
  }
  // The count of each role, because "the model saw four turns" and "the model saw one"
  // explain very different replies.
  LogEvent("SENT (request " + std::to_string(requestNumber + 1) + "): " +
           std::to_string(request.size()) + " message(s), " +
           std::to_string(totalChars) + " chars -- " + std::to_string(systemCount) +
           " system, " + std::to_string(userCount) + " user, " +
           std::to_string(assistantCount) + " assistant; history trimmed to " +
           std::to_string(kMaxHistoryMessages) + " message(s) max");

  // The clock the reply is measured against starts here, where the request is handed
  // over, not where it was typed: the logged duration should mean "what the backend
  // took".
  requestStarted = std::chrono::steady_clock::now();
}

bool DesignerSession::IsBusy() const {
  std::lock_guard<std::mutex> lock(transcriptMutex);
  for (const Exchange &exchange : exchanges) {
    if (!exchange.done) {
      return true;
    }
  }
  return false;
}

size_t DesignerSession::Size() const {
  std::lock_guard<std::mutex> lock(transcriptMutex);
  return exchanges.size();
}

DesignerSession::Exchange DesignerSession::Get(size_t index) const {
  std::lock_guard<std::mutex> lock(transcriptMutex);
  if (index >= exchanges.size()) return {};
  return exchanges[index];
}

std::vector<DesignerSession::Exchange> DesignerSession::Snapshot() const {
  std::lock_guard<std::mutex> lock(transcriptMutex);
  return exchanges;
}

void DesignerSession::Update(size_t index,
                             const std::function<void(Exchange &)> &write) {
  std::lock_guard<std::mutex> lock(transcriptMutex);
  if (index >= exchanges.size()) return;
  write(exchanges[index]);
}

size_t DesignerSession::Append(Exchange exchange) {
  std::lock_guard<std::mutex> lock(transcriptMutex);
  exchanges.push_back(std::move(exchange));
  return exchanges.size() - 1;
}

// ---------------------------------------------------------------------------
// Inspection
// ---------------------------------------------------------------------------

DesignerSession::Exchange DesignerSession::BeginInspection(size_t index) {
  Exchange finished;
  {
    std::lock_guard<std::mutex> lock(transcriptMutex);
    if (index >= exchanges.size()) return finished;
    finished = exchanges[index];
  }

  ++requestNumber;

  // How long the reply took, from the moment the request was handed to the backend. A
  // monotonic clock because this is a duration and a clock step would make it nonsense.
  // Without it a slow model and a stalled backend read identically in a transcript.
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - requestStarted);

  // The model's reply verbatim, including whatever prose surrounds the fence. This is the
  // thing a bad parse has to be diagnosed against, so it is recorded before anything is
  // extracted from it.
  LogEvent("MODEL REPLY (request " + std::to_string(requestNumber) + ", " +
           std::to_string(finished.answer.size()) + " bytes, " +
           std::to_string(elapsed.count()) + " ms" +
           (finished.fromUser ? ", answers a user request" : ", engine retry") + ")");
  Log(finished.answer);
  Log("\n");

  return finished;
}

void DesignerSession::Pump(const std::function<void(size_t)> &inspect) {
  if (!inspect) return;

  // The indexes are collected first: an inspector may append a retry, and iterating the
  // live vector while it grows would inspect the retry in the same frame it was sent.
  std::vector<size_t> pending;
  {
    std::lock_guard<std::mutex> lock(transcriptMutex);
    for (size_t i = 0; i < exchanges.size(); ++i) {
      if (!exchanges[i].inspected && exchanges[i].done) {
        pending.push_back(i);
      }
    }
  }

  for (const size_t index : pending) {
    inspect(index);
  }
}

bool DesignerSession::RetryRefused(int conversation, const std::string &reason,
                                     const std::string &artifactNoun, bool wasSet) {
  LogEvent("REFUSED: " + reason);
  const int attempts = CountAttempt(conversation);
  const int maxAttempts = MaxAttempts(conversation);

  if (attempts >= maxAttempts) {
    LogEvent("GIVING UP after " + std::to_string(attempts) + " attempt(s)");
    if (game != nullptr && game->debugLog) {
      game->debugLog->LogWarning(label + ": giving up on a " + artifactNoun +
                                 " after " + std::to_string(attempts) +
                                 " attempt(s); last reason: " + reason);
    }
    return false;
  }

  LogEvent("RETRYING (" + std::to_string(attempts) + " of " +
           std::to_string(maxAttempts) + " used)");

  const std::string prompt =
      "That " + artifactNoun + " was refused: " + reason +
      "\n\nSend the corrected " + artifactNoun +
      " as a complete ```json block, with designerNotes. Do not describe the fix in "
      "prose instead of sending it.";

  // The engine's own prompt is logged too, not just the user's: diagnosing a retry loop
  // means seeing the exact reason the model was handed.
  Log(prompt);
  Log("\n");

  return Send(prompt, conversation, /*fromUser=*/false, wasSet);
}

// ---------------------------------------------------------------------------
// Attempt budgets
// ---------------------------------------------------------------------------

int DesignerSession::Attempts(int conversation) const {
  if (!ValidConversation(conversation)) return 0;
  return conversations[conversation].attempts;
}

int DesignerSession::CountAttempt(int conversation) {
  if (!ValidConversation(conversation)) return 0;
  return ++conversations[conversation].attempts;
}

void DesignerSession::ResetAttempts(int conversation) {
  if (!ValidConversation(conversation)) return;
  conversations[conversation].attempts = 0;
}

void DesignerSession::SetMaxAttempts(int conversation, int maxAttempts) {
  if (!ValidConversation(conversation)) return;
  conversations[conversation].maxAttempts = maxAttempts;
}

int DesignerSession::MaxAttempts(int conversation) const {
  if (!ValidConversation(conversation)) return 0;
  const int configured = conversations[conversation].maxAttempts;
  if (configured > 0) return configured;
  return game != nullptr ? game->maxDesignAttempts() : 3;
}

// ---------------------------------------------------------------------------
// History
// ---------------------------------------------------------------------------

void DesignerSession::PushTurn(int conversation, const std::string &userText,
                               const std::string &assistantText) {
  if (!ValidConversation(conversation)) return;
  Conversation &active = conversations[conversation];
  active.history.push_back({"user", userText});
  // Absent rather than empty when the reply carried nothing: a failed transport has no
  // assistant turn to replay, and an empty one would read to the model as a silent
  // answer.
  if (!assistantText.empty()) {
    active.history.push_back({"assistant", assistantText});
  }
}

void DesignerSession::ClearHistory(int conversation) {
  if (!ValidConversation(conversation)) return;
  conversations[conversation].history.clear();
}

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

void DesignerSession::Log(const std::string &text) {
  if (text.empty()) return;

  if (logFilePath.empty()) {
    const std::filesystem::path directory = LogPaths::Subdir(logDirectory);
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)) {
      // Not fatal: a session with nowhere to write still works, it is just not recorded.
      // Reported through the game's log so the absence is explicable.
      if (game != nullptr && game->debugLog) {
        game->debugLog->LogWarning(label + ": could not create " + directory.string() +
                                   ": " + ec.message());
      }
      // Marks the attempt as failed so every later call re-tries rather than spinning on
      // an empty path forever.
      logFilePath = " ";
      return;
    }
    logFilePath =
        (directory / (TimeUtils::GetCurrentTimeString() + logSuffix)).string();
  }
  if (logFilePath == " ") return;

  std::ofstream out(logFilePath, std::ios::app);
  if (out.is_open()) {
    out << text;
  }
}

void DesignerSession::LogEvent(const std::string &text) {
  Log("\n=== [" + TimeUtils::GetCurrentTimeString() + "] " + text + "\n");
}

void DesignerSession::LogPromptFile(const std::string &path) {
  if (path.empty()) {
    Log("prompt file: (none configured -- the vocabulary below is all the model was "
        "given)\n");
    return;
  }
  std::ifstream file(path);
  if (!file.is_open()) {
    // Worth recording precisely: a silent fallback to "no instructions" is the
    // difference between a model that ignored its brief and one that never had it.
    Log("prompt file: " + path + "  -- COULD NOT BE READ\n");
    return;
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  const std::string text = buffer.str();
  int lines = 0;
  for (const char c : text) {
    if (c == '\n') ++lines;
  }
  Log("prompt file: " + path + "  (" + std::to_string(text.size()) + " bytes, " +
      std::to_string(lines) + " lines)\n");
  Log("--- file contents (the instructions; the vocabulary is appended below) ---\n");
  Log(text);
  if (!text.empty() && text.back() != '\n') Log("\n");
  Log("--- end of file ---\n");
}

// ---------------------------------------------------------------------------
// Speech
// ---------------------------------------------------------------------------

SpeechQueue *DesignerSession::Speech() const {
  if (game == nullptr || !game->speechQueue) {
    return nullptr;
  }
  return game->speechQueue.get();
}

bool DesignerSession::SpeechAvailable() const {
  SpeechQueue *speech = Speech();
  return speech != nullptr && speech->HasBackend();
}

void DesignerSession::StopSpeech() {
  if (SpeechQueue *speech = Speech()) {
    speech->Stop();
  }
}

void DesignerSession::SpeakFor(size_t index, const std::string &text) {
  SpeechQueue *speech = Speech();
  const SpeechClipId id = speech != nullptr ? speech->Enqueue(text) : kNoSpeechClip;

  {
    std::lock_guard<std::mutex> lock(transcriptMutex);
    if (index < exchanges.size()) {
      exchanges[index].speechId = id;
      // Kept whether or not a clip was created: it is what the bar's "Speak again" needs,
      // and the only record of what was actually read aloud.
      exchanges[index].spokenText = text;
    }
  }

  // Logged even when nothing was queued. "The engine was asked and produced no clip" is
  // exactly the fact worth having, and it is invisible otherwise.
  LogEvent(id == kNoSpeechClip ? "SPOKEN (no audio): " + text : "SPOKEN: " + text);
}

void DesignerSession::DrawSpeechBar(size_t index, Exchange &exchange) {
  SpeechQueue *speech = Speech();
  const bool speechAvailable = speech != nullptr && speech->HasBackend();
  const SpeechQueue::ClipState state =
      speech != nullptr ? speech->StateOf(exchange.speechId)
                        : SpeechQueue::ClipState::Unavailable;

  const bool preparing = state == SpeechQueue::ClipState::Preparing;
  const bool playing = state == SpeechQueue::ClipState::Playing;
  const bool paused = state == SpeechQueue::ClipState::Paused;
  const bool hasAudio = playing || paused || state == SpeechQueue::ClipState::Ready;

  ImGui::PushID(static_cast<int>(index));

  if (!hasAudio && !preparing) {
    // No audio for this reply: speech was off or muted when it arrived, the engine
    // produced nothing, or the cap dropped it. Three different causes, one useful action,
    // so the button is the same and the tooltip explains which it was.
    const bool canSpeak =
        speechAvailable && !speech->IsMuted() && !exchange.spokenText.empty();

    std::string why;
    if (state == SpeechQueue::ClipState::Failed) {
      why = "The speech engine produced no audio for this reply.";
    } else if (state == SpeechQueue::ClipState::Dropped) {
      why = "The audio for this reply was dropped to stay under the retention cap (the "
            "last " +
            std::to_string(SpeechQueue::kMaxRetainedClips) + " replies are kept).";
    } else if (!speechAvailable) {
      why = game != nullptr && !game->speechUnavailableReason.empty()
                ? game->speechUnavailableReason
                : std::string("No speech engine is available.");
    } else if (speech->IsMuted()) {
      why = "Speech is muted. Unmute to have this read aloud.";
    } else {
      why = "This reply was not spoken.";
    }

    ImGui::BeginDisabled(!canSpeak);
    if (ImGui::Button("Speak") && canSpeak) {
      exchange.speechId = speech->Enqueue(exchange.spokenText);
      std::lock_guard<std::mutex> lock(transcriptMutex);
      if (index < exchanges.size()) {
        exchanges[index].speechId = exchange.speechId;
      }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%s", why.c_str());
    }

    ImGui::PopID();
    return;
  }

  // Widths are measured from the font rather than guessed, because this bar has to fit
  // the transcript pane at any splitter position. The slider takes whatever is left, with
  // a floor so it never becomes unusable at a narrow width.
  const ImGuiStyle &style = ImGui::GetStyle();
  const float spacing = style.ItemSpacing.x;
  const float buttonWidth =
      std::max(ImGui::CalcTextSize("Pause").x, ImGui::CalcTextSize("Stop").x) +
      style.FramePadding.x * 2.0f;
  const float timeWidth = ImGui::CalcTextSize("00.0 / 00.0s").x;
  const float sliderWidth =
      std::max(60.0f, ImGui::GetContentRegionAvail().x - buttonWidth * 2.0f -
                          timeWidth - spacing * 3.0f);

  const float duration = speech->Duration(exchange.speechId);
  float position = speech->Position(exchange.speechId);

  ImGui::BeginDisabled(preparing);

  if (ImGui::Button(playing ? "Pause" : "Play", ImVec2(buttonWidth, 0.0f))) {
    speech->Toggle(exchange.speechId);
  }
  if (ImGui::IsItemHovered() && paused) {
    ImGui::SetTooltip("Resume from %.1fs.", static_cast<double>(position));
  }

  ImGui::SameLine();
  ImGui::SetNextItemWidth(sliderWidth);
  // Seeking as it is dragged. ImGui rewrites `position` from the mouse for the frames the
  // handle is held, and the queue is told each time, so the readout and the audio cannot
  // disagree about where playback is.
  if (ImGui::SliderFloat("##seek", &position, 0.0f, std::max(duration, 0.001f), "")) {
    speech->Seek(exchange.speechId, position);
  }
  if (ImGui::IsItemHovered() && duration > 0.0f) {
    ImGui::SetTooltip("Drag to move through the reading.");
  }

  ImGui::SameLine();
  if (preparing) {
    ImGui::TextDisabled("preparing...");
  } else {
    ImGui::Text("%.1f / %.1fs", static_cast<double>(position),
                static_cast<double>(duration));
  }

  ImGui::SameLine();
  // Stopping is only meaningful once something has been loaded, and it rewinds rather
  // than discards: the audio stays for another play.
  ImGui::BeginDisabled(!playing && !paused);
  if (ImGui::Button("Stop", ImVec2(buttonWidth, 0.0f))) {
    speech->StopClip(exchange.speechId);
  }
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Stop and rewind. The reading stays available.");
  }

  ImGui::EndDisabled();
  ImGui::PopID();
}

// ---------------------------------------------------------------------------
// The transcript
// ---------------------------------------------------------------------------

void DesignerSession::DrawTranscript(
    bool showThoughts, bool showJson, const std::string &emptyHint,
    const std::function<void(Exchange &)> &drawVerdict) {
  // Drawn from a copy taken under the lock, so the worker appending tokens cannot race
  // the render pass.
  std::vector<Exchange> snapshot;
  {
    std::lock_guard<std::mutex> lock(transcriptMutex);
    snapshot = exchanges;
  }

  if (snapshot.empty() && !emptyHint.empty()) {
    ImGui::TextWrapped("%s", emptyHint.c_str());
  }

  for (size_t i = 0; i < snapshot.size(); ++i) {
    // Non-const: DrawSpeechBar writes a fresh handle back when it re-speaks a reply. The
    // snapshot is this frame's copy of the transcript, and the write-through to `exchanges`
    // happens under the lock inside the bar.
    Exchange &exchange = snapshot[i];

    if (exchange.fromUser && !exchange.prompt.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.8f, 1.0f, 1.0f));
      ImGui::TextWrapped("You: %s", exchange.prompt.c_str());
      ImGui::PopStyleColor();
    } else if (!exchange.prompt.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.5f, 1.0f));
      ImGui::TextWrapped("Retry (%d): %s", static_cast<int>(i), exchange.prompt.c_str());
      ImGui::PopStyleColor();
    }

    if (!exchange.answer.empty()) {
      // Tinted while reasoning is shown, so it is obvious at a glance that this part is
      // the model working rather than its answer.
      const std::string shown = DesignerReply::DisplayAnswer(
          exchange.answer, showThoughts, showJson, hiddenMarker);
      if (showThoughts && shown.find("<think>") != std::string::npos) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.66f, 0.72f, 1.0f));
        ImGui::TextWrapped("%s", shown.c_str());
        ImGui::PopStyleColor();
      } else {
        ImGui::TextWrapped("%s", shown.c_str());
      }
    }
    if (!exchange.done) {
      ImGui::TextDisabled("...");
    }

    if (drawVerdict) {
      drawVerdict(exchange);
    }

    if (exchange.failed && !exchange.error.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
      ImGui::TextWrapped("Error: %s", exchange.error.c_str());
      ImGui::PopStyleColor();
    }

    // The transport bar, directly under the reply it belongs to.
    //
    // Only for replies that were actually spoken: an intermediate refusal that was about
    // to be retried never reached the engine, and a bar under it would be a control for
    // something that never happened. The user's own messages get no bar either.
    // `spokenText` is set by SpeakFor, which is what marks a reply as spoken -- including
    // the case where it was handed over but no audio came back.
    if (!exchange.fromUser && !exchange.spokenText.empty()) {
      DrawSpeechBar(i, exchange);
    }

    ImGui::Separator();
  }
}
