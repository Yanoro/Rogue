#include "DesignerChatWindow.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

#include "BlueprintExpansion.hpp"
#include "BlueprintRegistry.h"
#include "Components.h"
#include "DebugLog.h"
#include "Game.h"
#include "TimeUtils.hpp"
#include "imgui.h"

namespace {

// The language tag that opens the fenced block the designer must produce.
constexpr const char *kJsonFence = "```json";

// How many of the most recent messages are replayed to the model. Unbounded
// history is the one cost that grows without limit in a chat like this, and a
// design is a large object: after a handful of them every request would carry
// every blueprint ever made. The cap is generous enough that "change the door
// position" still has the previous design in view.
constexpr size_t kMaxHistoryMessages = 8;

}  // namespace

DesignerChatWindow::DesignerChatWindow(Game *game, AI *designerAI,
                                       std::string modelLabel,
                                       flecs::entity owner)
    : game(game), ai(designerAI), modelLabel(std::move(modelLabel)) {
  // One context per window instance, named so it cannot collide with an NPC's
  // ("npc-<id>") or the vision window's.
  contextId = "designer-chat-" + std::to_string(reinterpret_cast<uintptr_t>(this));
  if (game != nullptr) {
    systemPrompt = game->BuildDesignerSystemPrompt();
  }
}

void DesignerChatWindow::Log(const std::string &text) {
  if (text.empty()) return;

  if (logFilePath.empty()) {
    // logs/designer/, beside the NPC transcripts but in its own directory: the
    // designer is not an NPC, and mixing its conversations in with the villagers'
    // would make the folder harder to scan.
    std::error_code ec;
    std::filesystem::create_directories("logs/designer", ec);
    if (ec) {
      // Not fatal: a session with nowhere to write still works, it is just not
      // recorded. Reported through the game's log so the absence is explicable.
      if (game != nullptr && game->debugLog) {
        game->debugLog->LogWarning(
            "Designer: could not create logs/designer: " + ec.message());
      }
      // Marks the attempt as failed so every later call re-tries rather than
      // spinning on an empty path forever.
      logFilePath = " ";
      return;
    }
    logFilePath = "logs/designer/" + TimeUtils::GetCurrentTimeString() +
                  "-designer.txt";
  }
  if (logFilePath == " ") return;

  std::ofstream out(logFilePath, std::ios::app);
  if (out.is_open()) {
    out << text;
  }
}

void DesignerChatWindow::LogEvent(const std::string &text) {
  Log("\n=== [" + TimeUtils::GetCurrentTimeString() + "] " + text + "\n");
}

std::string DesignerChatWindow::ExtractJsonBlock(const std::string &text) {
  // The first fenced json block. Deliberately tolerant about what surrounds it,
  // because models like to add a closing remark, and strict about the fence,
  // because that is the one thing the prompt is unambiguous about.
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
    // An unterminated fence: take the rest. A model that forgot to close it has
    // usually still produced the object, and reporting "no JSON block" would be
    // a less useful error than whatever the parser says about the text.
    return text.substr(bodyStart + 1);
  }
  return text.substr(bodyStart + 1, close - bodyStart - 1);
}

void DesignerChatWindow::SendUserMessage(const std::string &prompt) {
  if (ai == nullptr || prompt.empty() || IsBusy()) {
    return;
  }

  // A new request restarts the attempt budget: the cap is per request, not per
  // session, or the third design of a session could never be retried.
  attempts = 0;

  Exchange ex;
  ex.prompt = prompt;
  ex.fromUser = true;

  size_t index = 0;
  {
    std::lock_guard<std::mutex> lock(transcriptMutex);
    exchanges.push_back(std::move(ex));
    index = exchanges.size() - 1;
  }

  // Logged before the request goes out, so a send that never gets a reply still
  // leaves a record of what was asked.
  LogEvent("USER REQUEST (attempt " + std::to_string(attempts + 1) +
           " of this request)");
  Log(prompt);
  Log("\n");

  GenerateResponse(index, prompt);
}

void DesignerChatWindow::GenerateResponse(size_t index, std::string prompt) {
  AI *backend = ai;
  if (backend == nullptr) {
    return;
  }

  // Snapshot the history on the main thread. The worker must not read `history`
  // while a later turn mutates it, and the request it is answering has to be
  // built from the conversation as it stood when the request was made.
  std::vector<ChatMessage> request;
  if (!systemPrompt.empty()) {
    request.push_back({"system", systemPrompt});
  }
  const size_t firstKept =
      history.size() > kMaxHistoryMessages ? history.size() - kMaxHistoryMessages : 0;
  for (size_t i = firstKept; i < history.size(); ++i) {
    request.push_back(history[i]);
  }
  request.push_back({"user", prompt});

  // The worker keeps the game loop responsive for providers whose generateStream
  // blocks. OllamaAI's does not -- it returns immediately and streams from its own
  // thread -- which is why completion is signalled by the empty-token sentinel and
  // never by this function returning.
  //
  // Nothing is written to the transcript from here: the stream callback runs on a
  // provider thread and fires once per token, so logging there would mean a file
  // open per token. The whole reply is written once, by InspectFinishedExchange on
  // the main thread.
  std::thread([this, index, request = std::move(request), backend]() {
    // A fresh provider-side context per request, because the history is sent
    // explicitly: leaving the previous request's tokens in place would replay the
    // conversation twice.
    backend->resetContext(contextId);

    std::string failure;
    const bool delivered = backend->generateStream(
        contextId, request,
        [this, index](const std::string &token) {
          std::lock_guard<std::mutex> lock(transcriptMutex);
          if (index >= exchanges.size()) {
            return;
          }
          Exchange &ex = exchanges[index];
          if (!token.empty()) {
            ex.answer += token;
            return;
          }
          // Empty token: every provider uses it as the end-of-stream sentinel.
          ex.done = true;
          if (ex.answer.empty()) {
            ex.failed = true;
            ex.error =
                "No answer came back. Check that the backend is running and "
                "that the model is available.";
          }
        });
    // generateStream's remaining parameters (a stop token and an image list) have
    // defaults, and the designer is text-only, so neither is passed.

    if (!delivered) {
      failure = "The designer backend could not accept the request.";
    }

    std::lock_guard<std::mutex> lock(transcriptMutex);
    if (index < exchanges.size()) {
      Exchange &ex = exchanges[index];
      ex.done = true;
      if (!failure.empty()) {
        ex.failed = true;
        ex.error = failure;
      }
      // The backend returned without ever sending the end-of-stream sentinel,
      // which is a provider bug rather than an empty answer. Marked done above so
      // the window cannot sit on a spinner forever, and given a reason so it is
      // not mistaken for a design that produced no cells.
      if (!ex.failed && ex.answer.empty() && failure.empty()) {
        ex.failed = true;
        ex.error =
            "The stream ended without a reply and without an end-of-stream "
            "signal.";
      }
    }
  }).detach();
}

bool DesignerChatWindow::IsBusy() {
  std::lock_guard<std::mutex> lock(transcriptMutex);
  for (const Exchange &ex : exchanges) {
    if (!ex.done) {
      return true;
    }
  }
  return false;
}

void DesignerChatWindow::InspectFinishedExchange(size_t index) {
  Exchange finished;
  {
    std::lock_guard<std::mutex> lock(transcriptMutex);
    if (index >= exchanges.size()) return;
    finished = exchanges[index];
  }

  ++requestNumber;

  // The model's reply verbatim, including whatever prose surrounds the fence. This
  // is the thing a bad parse has to be diagnosed against, so it is recorded
  // before anything is extracted from it.
  LogEvent("MODEL REPLY (request " + std::to_string(requestNumber) + ", " +
           std::to_string(finished.answer.size()) + " bytes" +
           (finished.fromUser ? ", answers a user request" : ", engine retry") + ")");
  Log(finished.answer);
  Log("\n");

  if (finished.failed) {
    LogEvent("TRANSPORT FAILURE: " + finished.error);
    std::lock_guard<std::mutex> lock(transcriptMutex);
    if (index < exchanges.size()) exchanges[index].inspected = true;
    history.push_back({"user", finished.prompt});
    return;
  }

  std::string reason;
  bool ok = false;
  std::string designId;
  std::string notes;
  std::string savedPath;
  bool overwrote = false;

  const std::string block = ExtractJsonBlock(finished.answer);
  if (block.empty()) {
    reason =
        "the reply had no ```json block. The blueprint must be inside one fenced "
        "block tagged json.";
  } else {
    nlohmann::json parsed;
    try {
      parsed = nlohmann::json::parse(block);
    } catch (const std::exception &e) {
      reason = std::string("the json block is not valid JSON: ") + e.what();
    }

    if (reason.empty()) {
      if (!parsed.is_object()) {
        reason = "the json block must be a JSON object describing one blueprint.";
      } else if (!parsed.contains("designerNotes") ||
                 !parsed["designerNotes"].is_string() ||
                 parsed["designerNotes"].get<std::string>().empty()) {
        // Required by the contract, and the reason it is required is the whole
        // point of this window: the model's own judgement of its design.
        reason =
            "the blueprint has no non-empty \"designerNotes\" string. Every design "
            "must carry your own thoughts on it.";
      } else {
        notes = parsed["designerNotes"].get<std::string>();
        // Removed before saving: it is not part of the schema, so writing it back
        // would put a designer-only field into content the rest of the engine
        // parses.
        parsed.erase("designerNotes");

        const std::string id =
            parsed.value("id", std::string());
        if (id.empty()) {
          reason = "the blueprint has no \"id\".";
        } else {
          BlueprintRegistry *registry = nullptr;
          if (auto *res = game != nullptr ? game->ecs.get<BlueprintRegistryResource>()
                                          : nullptr) {
            registry = res->registry;
          }

          Blueprint blueprint;
          std::string parseError;
          std::vector<std::string> warnings;
          BlueprintRegistry::ParseBlueprint(parsed, id, "(designer)", blueprint,
                                            parseError, warnings);
          if (!parseError.empty()) {
            reason = parseError;
          } else {
            // Expansion is checked before saving, not after: a design that parses
            // but produces no cells would otherwise be written to disk and appear
            // in the list as an empty building.
            const BlueprintExpansion::Expansion expansion =
                BlueprintExpansion::Expand(blueprint);
            if (!expansion.errors.empty()) {
              reason = expansion.errors.front();
              if (expansion.errors.size() > 1) {
                reason += " (and " + std::to_string(expansion.errors.size() - 1) +
                          " more problem(s))";
              }
            } else if (expansion.cells.empty()) {
              reason = "the design expands to no cells at all.";
            } else if (registry == nullptr) {
              reason = "there is no blueprint registry to save to.";
            } else {
              const BlueprintRegistry::SaveResult saved =
                  registry->SaveBlueprint(blueprint);
              if (!saved.ok) {
                reason = saved.error;
              } else {
                ok = true;
                designId = blueprint.id;
                savedPath = saved.path;
                overwrote = saved.overwrote;
              }
            }
          }
        }
      }
    }
  }

  {
    std::lock_guard<std::mutex> lock(transcriptMutex);
    if (index < exchanges.size()) {
      Exchange &ex = exchanges[index];
      ex.inspected = true;
      ex.designOk = ok;
      ex.designId = designId;
      ex.savedPath = savedPath;
      ex.overwrote = overwrote;
      ex.designerNotes = notes;
      ex.failureReason = reason;
    }
  }

  // The conversation both sides saw, so a follow-up can refer to this design. The
  // assistant turn is stored as the raw reply, and the capped replay is what keeps
  // a large blueprint from accumulating.
  history.push_back({"user", finished.prompt});
  history.push_back({"assistant", finished.answer});

  if (ok) {
    LogEvent("SAVED: " + designId + " -> " + savedPath +
             (overwrote ? " (replaced an existing design)" : " (new design)"));
    if (!notes.empty()) {
      Log("designerNotes: " + notes + "\n");
    }
    if (game != nullptr && game->debugLog) {
      game->debugLog->LogInfo("Designer: saved blueprint '" + designId + "' to " +
                              savedPath + (overwrote ? " (replaced)" : ""));
    }
    attempts = 0;
    return;
  }

  // Refused: say why, in the same conversation, and try again. Bounded so a model
  // that cannot satisfy the contract cannot spend the whole budget.
  LogEvent("REFUSED: " + reason);
  ++attempts;
  const int maxAttempts = game != nullptr ? game->maxDesignAttempts() : 3;
  if (attempts >= maxAttempts) {
    LogEvent("GIVING UP after " + std::to_string(attempts) + " attempt(s)");
    if (game != nullptr && game->debugLog) {
      game->debugLog->LogWarning("Designer: giving up after " +
                                 std::to_string(attempts) +
                                 " attempt(s); last reason: " + reason);
    }
    return;
  }

  LogEvent("RETRYING (" + std::to_string(attempts) + " of " +
           std::to_string(maxAttempts) + " used)");

  Exchange retry;
  retry.prompt =
      "That design was refused: " + reason +
      "\n\nSend the corrected blueprint as a complete ```json block, with "
      "designerNotes. Do not describe the fix in prose instead of sending it.";
  retry.fromUser = false;

  size_t retryIndex = 0;
  {
    std::lock_guard<std::mutex> lock(transcriptMutex);
    exchanges.push_back(std::move(retry));
    retryIndex = exchanges.size() - 1;
  }

  // The engine's own prompt is logged too, not just the user's: diagnosing a retry
  // loop means seeing the exact reason the model was handed.
  Log(retry.prompt);
  Log("\n");

  GenerateResponse(retryIndex, exchanges[retryIndex].prompt);
}

void DesignerChatWindow::Draw() {
  bool isOpen = true;
  ImGui::SetNextWindowSize(ImVec2(760, 560), ImGuiCond_FirstUseEver);
  ImGui::Begin("Designer", &isOpen, ImGuiWindowFlags_None);
  if (!isOpen) {
    game->designerWindowEntity.remove<ActiveWindow>();
    ImGui::End();
    return;
  }

  ImGui::TextDisabled("Backend: %s", modelLabel.empty() ? "(none)" : modelLabel.c_str());
  ImGui::SameLine();
  ImGui::TextDisabled("| %d of %d attempts used", attempts,
                      game != nullptr ? game->maxDesignAttempts() : 0);
  ImGui::Separator();

  if (ai == nullptr) {
    ImGui::TextWrapped(
        "No designer backend is configured, so nothing can be sent. Set "
        "\"designer\" in settings.json (see SETTINGS.md) or leave "
        "\"useMainModel\" on to share the NPC backend.");
    ImGui::End();
    return;
  }

  if (ImGui::Button("Load existing design")) {
    // Copies the currently selected design in the Blueprints window into the
    // prompt, which is how "change this one" is asked without pasting JSON by
    // hand.
    BlueprintRegistry *registry = nullptr;
    if (auto *res = game->ecs.get<BlueprintRegistryResource>()) {
      registry = res->registry;
    }
    const Blueprint *selected =
        registry != nullptr && !game->selectedBlueprintId.empty()
            ? registry->Get(game->selectedBlueprintId)
            : nullptr;
    if (selected != nullptr) {
      const std::string json =
          BlueprintRegistry::WriteBlueprint(*selected).dump(2);
      const std::string message =
          "Here is my current " + selected->name + ":\n\n```json\n" + json +
          "\n```";
      std::snprintf(inputBuffer, sizeof(inputBuffer), "%s",
                    message.substr(0, sizeof(inputBuffer) - 1).c_str());
    }
  }
  ImGui::SameLine();
  ImGui::TextDisabled(
      "(select a design in the Blueprints window first)");

  // The transcript. Drawn from a copy taken under the lock, so the worker
  // appending tokens cannot race the render pass.
  ImGui::BeginChild("designer_transcript", ImVec2(0, -70), true);
  std::vector<Exchange> snapshot;
  {
    std::lock_guard<std::mutex> lock(transcriptMutex);
    snapshot = exchanges;
  }

  if (snapshot.empty()) {
    ImGui::TextWrapped(
        "Ask for a building. The designer replies with a blueprint and its own "
        "notes on it; a valid design is saved into data/blueprints/ and appears "
        "in the Blueprints window straight away.");
  }

  bool needsInspection = false;
  for (size_t i = 0; i < snapshot.size(); ++i) {
    const Exchange &ex = snapshot[i];
    if (!ex.inspected && ex.done) {
      needsInspection = true;
    }

    if (ex.fromUser && !ex.prompt.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.8f, 1.0f, 1.0f));
      ImGui::TextWrapped("You: %s", ex.prompt.c_str());
      ImGui::PopStyleColor();
    } else if (!ex.prompt.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.5f, 1.0f));
      ImGui::TextWrapped("Retry (%d): %s", static_cast<int>(i), ex.prompt.c_str());
      ImGui::PopStyleColor();
    }

    if (!ex.answer.empty()) {
      ImGui::TextWrapped("%s", ex.answer.c_str());
    }
    if (!ex.done) {
      ImGui::TextDisabled("...");
    }

    if (ex.inspected) {
      if (ex.designOk) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 1.0f, 0.4f, 1.0f));
        ImGui::TextWrapped("Saved '%s' to %s%s", ex.designId.c_str(),
                           ex.savedPath.c_str(),
                           ex.overwrote ? " (replaced an existing design)" : "");
        ImGui::PopStyleColor();
        if (!ex.designerNotes.empty()) {
          ImGui::TextDisabled("Designer's notes:");
          ImGui::TextWrapped("%s", ex.designerNotes.c_str());
        }
      } else if (!ex.failureReason.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.4f, 1.0f));
        ImGui::TextWrapped("Refused: %s", ex.failureReason.c_str());
        ImGui::PopStyleColor();
      }
    }
    if (ex.failed && !ex.error.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
      ImGui::TextWrapped("Error: %s", ex.error.c_str());
      ImGui::PopStyleColor();
    }
    ImGui::Separator();
  }
  ImGui::EndChild();

  // Inspection runs on the main thread, after the transcript is drawn, and starts
  // any retry itself. Doing it on the worker would mean validating and saving
  // content from a background thread while the ECS is mid-frame.
  if (needsInspection) {
    for (size_t i = 0; i < snapshot.size(); ++i) {
      if (!snapshot[i].inspected && snapshot[i].done) {
        InspectFinishedExchange(i);
      }
    }
  }

  const bool busy = IsBusy();
  ImGui::BeginDisabled(busy);
  const bool send = ImGui::Button("Send");
  ImGui::SameLine();
  ImGui::TextDisabled("Enter to send, Shift+Enter for a newline");
  ImGui::EndDisabled();

  ImGui::SetNextItemWidth(-1.0f);
  const bool submitted = ImGui::InputTextMultiline(
      "##designer_input", inputBuffer, sizeof(inputBuffer), ImVec2(-1.0f, 54.0f),
      ImGuiInputTextFlags_EnterReturnsTrue |
          ImGuiInputTextFlags_CtrlEnterForNewLine);

  if ((send || submitted) && !busy) {
    std::string prompt(inputBuffer);
    // Trim the whitespace an Enter press leaves behind.
    while (!prompt.empty() && (prompt.back() == '\n' || prompt.back() == ' ')) {
      prompt.pop_back();
    }
    if (!prompt.empty()) {
      inputBuffer[0] = '\0';
      SendUserMessage(prompt);
    }
  }

  ImGui::End();
}
