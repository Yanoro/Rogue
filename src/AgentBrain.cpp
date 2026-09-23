#include "AgentBrain.h"
#include "AI.h"
#include "AgentContextWindow.h"
#include "Components.h"
#include "DebugLog.h"
#include "Map.h"
#include "PathFinding.h"
#include "StringUtils.hpp"
#include "TradeGrammar.hpp"
#include "Trading.h"
#include "flecs.h"

#include <any>
#include <iostream>
#include <memory>
#include <regex>
#include <thread>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <filesystem>
#include "TimeUtils.hpp"


TalkAction::TalkAction(flecs::entity sourceEntity, std::string targetName, ConversationState state)
    : state(state), targetName(targetName) {
  bool found = false;
  std::string myName = sourceEntity.get<DisplayName>()->name;

  if (StringUtils::EqualsIgnoreCase(myName, targetName)) {
    isTalkingToSelf = true;
    return;
  }

  sourceEntity.world().filter<DisplayName, AgentBrainWrapper>().each(
      [&](flecs::entity e, const DisplayName &name, AgentBrainWrapper & /*wrapper*/) {
        if (StringUtils::EqualsIgnoreCase(name.name, targetName)) {
          this->targetEntity = e;
          found = true;
        }
      });

  AgentBrain *sourceBrain = sourceEntity.get_mut<AgentBrainWrapper>()->agBrain.get();
  if (state == ConversationState::Talking) {
    sourceBrain->pushContext(
        "user",
        "System: You are now talking to " + targetName +
            ". What do you say? You can negotiate a trade with [OFFER <what "
            "you give> FOR <what you want>], for example [OFFER 3 Iron Ingot "
            "FOR 8 Flour] or [OFFER 2 Flour, 5 Bronze Coins FOR 3 Iron Ingot]. "
            "To leave the conversation, append [EXIT] to the end of your "
            "goodbye message, for example: I have to go now, goodbye! [EXIT]\n");
  } else {
    sourceBrain->pushContext(
        "user",
        "System: " + targetName +
            " approaches you to talk and is currently speaking. When it is "
            "your turn you can negotiate a trade, and you can append [EXIT] to "
            "the end of your goodbye message (for example: Talk to you later! "
            "[EXIT]) to leave the conversation.\n");
  }

  if (state == ConversationState::Talking && found) {
    AgentBrain *targetBrain = targetEntity.get_mut<AgentBrainWrapper>()->agBrain.get();
    targetBrain->ForceInterruptAndPush(
        std::make_unique<TalkAction>(targetEntity, myName, ConversationState::Listening)
    );
  }
}

namespace {

// Verbs the conversation parser accepts while two agents are talking. Anything
// else keeps the "stay in character" rejection, so allowing trade commands did
// not turn a conversation into free-for-all command spam.
bool IsConversationVerb(const std::string &verb) {
  return verb == "OFFER" || verb == "COUNTER_OFFER" || verb == "ACCEPT" ||
         verb == "DECLINE" || verb == "INVENTORY";
}

// Consecutive free-look [INVENTORY] calls allowed within a single turn.
constexpr int kMaxConsecutivePeeks = 3;

// Shown whenever an offer is live, so the model is reminded of the exact reply
// verbs -- and of the multi-item syntax -- at the moment it has to choose one.
constexpr const char *kTradeReplyHint =
    "Reply with [ACCEPT] to agree, [DECLINE] to refuse, [COUNTER_OFFER <what "
    "you give> FOR <what you receive>] to propose different terms (separate "
    "multiple items with commas, e.g. [COUNTER_OFFER 2 Flour, 5 Bronze Coins "
    "FOR 3 Iron Ingot]), or [EXIT] to decline the proposal and end the conversation.";

// The canonical way to write a multi-item offer. Both agents in the first live
// session invented "+" as a separator and were rejected for it, so the syntax is
// restated wherever an offer can fail.
std::string OfferSyntaxHint(const std::string &verb) {
  return "Write each item as an optional count followed by its name, and "
         "separate multiple items with commas, for example [" +
         verb + " 2 Flour, 5 Bronze Coins FOR 3 Iron Ingot].";
}

// "you give 2 Flour, 5 Bronze Coins; you ask for 3 Iron Ingot"
std::string DescribeTerms(const TradeGrammar::ParsedOffer &parsed) {
  return "you give " + TradeGrammar::Format(parsed.give) + "; you ask for " +
         TradeGrammar::Format(parsed.receive);
}

// Turns a shortfall into readable text. "You have no item called X" and "you are
// short N X" are deliberately different sentences: conflating the two is what
// made a mis-parsed list look like a broken inventory.
std::string DescribeShortfall(const Trading::ShortfallInfo &shortfall) {
  std::string text;
  for (const std::string &name : shortfall.unknownItems) {
    if (!text.empty()) text += ", ";
    text += "you have no item called '" + name + "'";
  }
  for (const ItemStack &stack : shortfall.insufficientStacks) {
    if (!text.empty()) text += ", ";
    text += "you are short " + std::to_string(stack.count) + " " + stack.item;
  }
  if (text.empty()) {
    text = "you cannot cover that";
  }
  return text;
}

} // namespace

std::string TalkAction::getActionContext() const {
  // Prefer the live partner's name over the spelling the model used, so a
  // rename shows up in the label. Self-talk never resolves an entity.
  std::string partner = targetName;
  if (targetEntity.is_alive() && targetEntity.has<DisplayName>()) {
    partner = targetEntity.get<DisplayName>()->name;
  }

  if (isTalkingToSelf) {
    return partner + " (self)";
  }

  // A listener stores the person talking *to* it, so the partner is the same
  // field in both directions; only whose turn it is differs.
  switch (state) {
  case ConversationState::Talking:
    return partner + " (speaking)";
  case ConversationState::Listening:
    return partner + " (listening)";
  case ConversationState::Ended:
    return partner + " (ending)";
  }
  return partner;
}

ActionStatus TalkAction::update(float deltaTime, flecs::entity entity) {
  // "Stop brain" has to hold inside a conversation too. TalkAction is the only
  // action that drives the model itself, so without this guard a stopped pair
  // keeps talking, trading and spending requests until one of them [EXIT]s:
  // AgentBrain::update only consults isStopped once the action queue is empty,
  // and while a conversation is running it never is.
  if (AgentBrainWrapper *wrapper = entity.get_mut<AgentBrainWrapper>()) {
    if (wrapper->agBrain && wrapper->agBrain->isStopped) {
      return ActionStatus::Doing;
    }
  }

  // Waiting out the backoff after an empty response before asking again. The
  // AIRequest was already dropped, so a fresh one is only created once this
  // reaches zero.
  if (retryCooldownMs > 0.0f) {
    retryCooldownMs -= deltaTime;
    return ActionStatus::Doing;
  }

  if (state == ConversationState::Talking) {
    const AIRequest *request = entity.get<AIRequest>();
    if (request == nullptr) {
      std::string context = entity.get<AgentBrainWrapper>()->agBrain->getContext();
      entity.set<AIRequest>({"", false, "", false});
    } else if (request->finished) {
      
      if (!targetEntity.is_alive() || !targetEntity.has<AgentBrainWrapper>()) {
        // The partner is gone, so no offer can survive them.
        Trading::ClearOffersBetween(entity, targetEntity);
        entity.remove<AIRequest>();
        return ActionStatus::Failed;
      }

      bool targetReady = false;
      TalkAction *targetTalk = nullptr;
      AgentBrain *targetBrain = targetEntity.get_mut<AgentBrainWrapper>()->agBrain.get();
      
      if (targetBrain && targetBrain->getCurrentAction()) {
        targetTalk = dynamic_cast<TalkAction *>(targetBrain->getCurrentAction());
        if (targetTalk && targetTalk->state == ConversationState::Listening) {
          targetReady = true;
        }
      }

      // We ONLY swap states and transfer the message if they are ready!
      // Otherwise, we just wait and check again next frame.
        if (targetReady) {
          // An empty response is never dialogue. It means the provider rejected
          // the request or the stream died, so flag it and back off before
          // retrying. The "stay in character" warning further down is for a
          // model that answered with no usable words, which is a different
          // problem from the provider not answering at all.
          if (request->pendingResponse.empty()) {
            std::string who = entity.has<DisplayName>()
                                  ? entity.get<DisplayName>()->name
                                  : std::string("NPC");
            entity.get<AgentBrainWrapper>()->agBrain->logWarning(
                "Empty AI response during conversation for " + who +
                " - retrying");
            retryCooldownMs = DEFAULT_EMPTY_RESPONSE_RETRY_MS;
            entity.remove<AIRequest>();
            return ActionStatus::Doing;
          }

        std::string response = request->pendingResponse;

        // Strip out <think> tags so they aren't spoken out loud
        std::regex thinkRegex(R"(<think>[\s\S]*?<\/think>)",
                              std::regex_constants::icase);
        response = std::regex_replace(response, thinkRegex, "");
        response = StringUtils::Trim(response);

        // [EXIT] ends the conversation and also declines any live offer.
        bool exiting = false;
        std::regex exitRegex(R"(\[EXIT\])", std::regex_constants::icase);
        if (std::regex_search(response, exitRegex)) {
          exiting = true;
          response = std::regex_replace(response, exitRegex, "");
        }

        // Pull the commands out and remove them from the spoken text, so
        // "Fine, [ACCEPT]" reaches the partner as "Fine,".
        struct Command {
          std::string verb;
          std::string args;
        };
        std::vector<Command> commands;
        static const std::regex commandRegex(R"(\[\s*([A-Za-z_]+)([^\]]*)\])");
        for (auto it = std::sregex_iterator(response.begin(), response.end(),
                                            commandRegex);
             it != std::sregex_iterator(); ++it) {
          Command command;
          command.verb = StringUtils::ToUpper((*it)[1].str());
          command.args = StringUtils::Trim((*it)[2].str());
          commands.push_back(command);
        }
        response = std::regex_replace(response, commandRegex, "");
        const std::string spoken = StringUtils::Trim(response);

        AgentBrain *selfBrain = entity.get_mut<AgentBrainWrapper>()->agBrain.get();
        const std::string myName = entity.has<DisplayName>()
                                       ? entity.get<DisplayName>()->name
                                       : std::string("Someone");

        // Send the speaker back to the model with a warning, WITHOUT passing the
        // turn. That is what enforces "a pending offer must be answered".
        auto rePrompt = [&](const std::string &warning) {
          selfBrain->pushContext("user", warning);
          entity.remove<AIRequest>();
          return ActionStatus::Doing;
        };

        auto inventoryBlock = [&]() {
          std::string listing = Trading::DescribeInventory(entity);
          return listing.empty() ? std::string("nothing.\n") : listing;
        };

        auto passTurnToPartner = [&]() {
          consecutivePeeks = 0;
          this->state = ConversationState::Listening;
          targetTalk->state = ConversationState::Talking;
          entity.remove<AIRequest>();
          return ActionStatus::Doing;
        };

        if (spoken.empty() && commands.empty() && !exiting) {
          return rePrompt(
              "System: Warning - you must stay in character during a TalkAction. "
              "Provide conversational dialogue, or append [EXIT] to your message "
              "to end the conversation.\n");
        }

        if (exiting) {
          Trading::ClearOffersBetween(entity, targetEntity);
          if (!spoken.empty()) {
            targetBrain->appendContext("user", myName + " says: " + spoken + "\n");
          }
          entity.remove<AIRequest>();
          targetTalk->state = ConversationState::Ended;
          return ActionStatus::Done;
        }

        if (commands.size() > 1) {
          return rePrompt(
              "System: Warning - issue only one command per message. Use "
              "[OFFER ...], [ACCEPT], [DECLINE], [COUNTER_OFFER ...], "
              "[INVENTORY] or [EXIT].\n");
        }
        for (const Command &command : commands) {
          if (!IsConversationVerb(command.verb)) {
            return rePrompt(
                "System: Warning - you must stay in character during a "
                "TalkAction. Speak naturally, or use only the trade commands "
                "[OFFER ...], [ACCEPT], [DECLINE], [COUNTER_OFFER ...] and "
                "[INVENTORY]. Append [EXIT] to your goodbye to end the "
                "conversation.\n");
          }
        }

        const std::string verb = commands.empty() ? "" : commands.front().verb;
        const std::string args = commands.empty() ? "" : commands.front().args;

        // A free look at your own inventory: never consumes the turn, and never
        // counts as answering a pending offer.
        if (verb == "INVENTORY") {
          if (consecutivePeeks >= kMaxConsecutivePeeks) {
            return rePrompt(
                "System: You have already checked your inventory " +
                std::to_string(kMaxConsecutivePeeks) +
                " times in a row. Answer what was said, or use [OFFER ...], "
                "[ACCEPT], [DECLINE], [COUNTER_OFFER ...] or [EXIT].\n");
          }
          consecutivePeeks++;
          const std::string listing = Trading::DescribeInventory(entity);
          selfBrain->appendContext(
              "user", listing.empty()
                          ? "System: Your inventory is empty.\n"
                          : "System: You are currently holding:\n" + listing);
          entity.remove<AIRequest>();
          return ActionStatus::Doing;
        }

        const TradeOffer *pending = Trading::FindOffer(targetEntity, entity);

        // ---- Being offered to: ACCEPT, DECLINE, COUNTER_OFFER or EXIT --------
        if (pending != nullptr) {
          if (verb.empty()) {
            return rePrompt("System: " + targetName +
                            "'s offer is still on the table and you must answer "
                            "it. " + kTradeReplyHint + "\n");
          }
          if (verb == "OFFER") {
            return rePrompt(
                "System: A counter-offer from " + targetName +
                " is already pending, so a plain [OFFER] cannot be used. Use "
                "[COUNTER_OFFER <what you give> FOR <what you receive>], "
                "[DECLINE], [ACCEPT] or [EXIT].\n");
          }
          if (verb == "ACCEPT") {
            // Copy the terms out before anything mutates the component.
            const TradeOffer offer = *pending;
            Trading::ShortfallInfo shortfall;
            const Trading::TradeOutcome outcome =
                Trading::ExecuteTrade(targetEntity, entity, offer, shortfall);
            if (outcome == Trading::TradeOutcome::ResponderCannotPay) {
              return rePrompt(
                  "System: You cannot accept that offer - " +
                  DescribeShortfall(shortfall) + ". You are holding:\n" +
                  inventoryBlock() +
                  "Refuse it with [DECLINE] or propose different terms with "
                  "[COUNTER_OFFER ...].\n");
            }
            if (outcome == Trading::TradeOutcome::OffererCannotPay) {
              Trading::ClearOffersBetween(entity, targetEntity);
              targetBrain->appendContext(
                  "user",
                  "System: That trade could not be completed and the offer has "
                  "been withdrawn.\n");
              return rePrompt(
                  "System: That trade could not be completed and the offer has "
                  "been withdrawn. Continue the conversation.\n");
            }

            Trading::ClearOffersBetween(entity, targetEntity);
            if (!spoken.empty()) {
              targetBrain->appendContext("user",
                                         myName + " says: " + spoken + "\n");
            }
            selfBrain->appendContext(
                "user", "System: Trade agreed. You gave " +
                            TradeGrammar::Format(offer.receive) +
                            " and received " + TradeGrammar::Format(offer.give) +
                            ".\n");
            targetBrain->appendContext(
                "user", "System: Trade agreed. You gave " +
                            TradeGrammar::Format(offer.give) +
                            " and received " +
                            TradeGrammar::Format(offer.receive) + ".\n");
            return passTurnToPartner();
          }
          if (verb == "DECLINE") {
            Trading::ClearOffersBetween(entity, targetEntity);
            if (!spoken.empty()) {
              targetBrain->appendContext("user",
                                         myName + " says: " + spoken + "\n");
            }
            targetBrain->appendContext(
                "user", "System: " + myName + " declined your offer.\n");
            return passTurnToPartner();
          }

          // verb == "COUNTER_OFFER"
          TradeGrammar::ParsedOffer parsed;
          const TradeGrammar::ParseResult result =
              TradeGrammar::Parse(args, parsed);
          if (result != TradeGrammar::ParseResult::Ok) {
            return rePrompt("System: Your counter-offer could not be read - " +
                            TradeGrammar::Explain(result) + ". " +
                            OfferSyntaxHint("COUNTER_OFFER") +
                            " The offer from " + targetName +
                            " still stands.\n");
          }
          Trading::CanonicalizeNames(entity, parsed.give);
          Trading::CanonicalizeNames(entity, parsed.receive);
          Trading::ShortfallInfo shortfall;
          if (!Trading::CanAfford(entity, parsed.give, shortfall)) {
            return rePrompt(
                "System: You cannot offer that - " +
                DescribeShortfall(shortfall) +
                ". I read your counter-offer as: " + DescribeTerms(parsed) +
                ". You are holding:\n" + inventoryBlock() +
                OfferSyntaxHint("COUNTER_OFFER") + " The offer from " +
                targetName + " still stands.\n");
          }
          // Only a well-formed, affordable counter replaces the live offer: a
          // formatting slip must never destroy the negotiation state.
          Trading::ClearOffersBetween(entity, targetEntity);
          Trading::SetOffer(entity, targetEntity, parsed.give, parsed.receive);
          selfBrain->appendContext(
              "user", "System: You countered " + targetName + ": you give " +
                          TradeGrammar::Format(parsed.give) + "; you ask for " +
                          TradeGrammar::Format(parsed.receive) + ".\n");
          if (!spoken.empty()) {
            targetBrain->appendContext("user",
                                       myName + " says: " + spoken + "\n");
          }
          targetBrain->appendContext(
              "user",
              "System: " + myName + " counters your offer. You would give: " +
                  TradeGrammar::Format(parsed.receive) +
                  ". You would receive: " +
                  TradeGrammar::Format(parsed.give) + ".\n" + kTradeReplyHint +
                  "\n");
          return passTurnToPartner();
        }

        // ---- Nothing pending: ordinary dialogue, or a fresh offer ------------
        if (verb.empty()) {
          if (!spoken.empty()) {
            targetBrain->appendContext("user", myName + " says: " + spoken + "\n");
          }
          return passTurnToPartner();
        }

        if (verb == "OFFER") {
          TradeGrammar::ParsedOffer parsed;
          const TradeGrammar::ParseResult result =
              TradeGrammar::Parse(args, parsed);
          if (result != TradeGrammar::ParseResult::Ok) {
            return rePrompt("System: Your offer could not be read - " +
                            TradeGrammar::Explain(result) + ". " +
                            OfferSyntaxHint("OFFER") + "\n");
          }
          Trading::CanonicalizeNames(entity, parsed.give);
          Trading::CanonicalizeNames(entity, parsed.receive);
          Trading::ShortfallInfo shortfall;
          if (!Trading::CanAfford(entity, parsed.give, shortfall)) {
            return rePrompt("System: You cannot offer that - " +
                            DescribeShortfall(shortfall) +
                            ". I read your offer as: " + DescribeTerms(parsed) +
                            ". You are holding:\n" + inventoryBlock() +
                            OfferSyntaxHint("OFFER") + "\n");
          }
          Trading::SetOffer(entity, targetEntity, parsed.give, parsed.receive);
          selfBrain->appendContext(
              "user", "System: You offered " + targetName + ": you give " +
                          TradeGrammar::Format(parsed.give) + "; you ask for " +
                          TradeGrammar::Format(parsed.receive) + ".\n");
          if (!spoken.empty()) {
            targetBrain->appendContext("user", myName + " says: " + spoken + "\n");
          }
          targetBrain->appendContext(
              "user",
              "System: " + myName + " offers a trade. You would give: " +
                  TradeGrammar::Format(parsed.receive) +
                  ". You would receive: " +
                  TradeGrammar::Format(parsed.give) + ".\n" + kTradeReplyHint +
                  "\n");
          return passTurnToPartner();
        }

        // ACCEPT / DECLINE / COUNTER_OFFER with nothing on the table.
        const std::string what = verb == "ACCEPT"    ? "accept"
                                 : verb == "DECLINE" ? "decline"
                                                     : "counter";
        return rePrompt("System: There is no offer to " + what +
                        " right now. Use [OFFER <what you give> FOR <what you "
                        "want>] to propose a trade, or keep talking.\n");
      }
    }
  } else if (state == ConversationState::Listening) {
    if (!targetEntity.is_alive() || !targetEntity.has<AgentBrainWrapper>()) {
      Trading::ClearOffersBetween(entity, targetEntity);
      return ActionStatus::Failed;
    }
    
    // Clear any pending AIRequest that was dispatched before we started listening
    // to prevent the AI from responding to the conversation with a stale command
    if (entity.has<AIRequest>()) {
      entity.remove<AIRequest>();
    }
  } else if (state == ConversationState::Ended) {
    return ActionStatus::Done;
  }

  return ActionStatus::Doing;
}

ActionStatus TalkAction::handleInterruption(flecs::entity entity) {
  // An interrupted conversation takes any live offer down with it, so no stale
  // offer can be accepted later by a partner who has moved on.
  Trading::ClearOffersBetween(entity, targetEntity);
  if (entity.has<AIRequest>()) {
    entity.remove<AIRequest>();
  }
  return ActionStatus::Interrupted;
}

void TalkAction::resume(flecs::entity) {
  // update() will naturally resume the conversation by re-requesting AI input
}

std::string TalkAction::getSuccessMessage() {
  return "System: You are done talking to " + targetName + ", what's next?\n";
}

std::string TalkAction::getFailureMessage() {
  if (isTalkingToSelf) {
    return "System: You cannot talk to yourself. Please choose another character or use a different command.\n";
  }
  return "System: The person you were trying to talk to is no longer available.\n";
}

AgentBrain::AgentBrain(flecs::entity entity, std::string name, DebugLog *debugLog)
    : entity(entity), debugLog(debugLog) {
  entity.set<WindowOnClick>({WindowType::NPCContextWindowType});
  entity.set_name(name.c_str());

  std::filesystem::create_directories("logs");
  logFilePath = "logs/" + TimeUtils::GetStartTimeString() + "-" + name + ".txt";
}

void AgentBrain::ForceInterruptAndPush(std::unique_ptr<AgentAction> action) {
  interruptCurrentAction();
  currentAction = std::move(action);
}

#include "GlobalCommandRegistry.h"

ActionThunk AgentBrain::ParseMessageCommand(std::string msg) {
  // Strip out <think> tags before parsing to avoid matching thoughts
  std::regex thinkRegex(R"(<think>[\s\S]*?<\/think>)", std::regex_constants::icase);
  std::string strippedMsg = std::regex_replace(msg, thinkRegex, "");

  // Check for multiple commands
  static std::regex anyCmdRegex(R"(\[[A-Z_]+.*?\])", std::regex_constants::icase);
  auto words_begin = std::sregex_iterator(strippedMsg.begin(), strippedMsg.end(), anyCmdRegex);
  auto words_end = std::sregex_iterator();
  if (std::distance(words_begin, words_end) > 1) {
    appendContext("user", "System: Warning - you issued multiple commands at once. You must only issue one command at a time.\n");
    return [](flecs::entity) { return std::make_unique<InvalidAction>(); };
  }

  for (const auto& cmd : GlobalCommandRegistry::GetCommands()) {
    std::smatch match;
    if (std::regex_search(strippedMsg, match, cmd.pattern)) {
      if (cmd.format != "[GENERIC_INTERACT]") {
        if (entity.has<InteractionTarget>()) {
          entity.remove<InteractionTarget>();
        }
      }
      return cmd.createAction(match);
    }
  }

  return [](flecs::entity) { return std::make_unique<InvalidAction>(); };
}

std::string AgentBrain::getContext() const {
  std::string result = "";
  auto ctx = entity.get<NPCContext>();
  if (ctx) {
    for (const auto& msg : ctx->history) {
      if (msg.role == "assistant") result += "You: " + msg.content;
      else result += msg.content;
    }
  }
  return result;
}

void AgentBrain::appendContext(const std::string &role, const std::string &text) {
  recordContext(role, text, true);
}

void AgentBrain::pushContext(const std::string &role, const std::string &text) {
  recordContext(role, text, false);
}

void AgentBrain::recordAssistantStream(const std::string &token) {
  if (!token.empty()) {
    recordContext("assistant", token, true);
    return;
  }

  // End of stream: close the reply with a newline so the next transcript entry
  // starts on its own line instead of butting up against the assistant's text.
  auto ctx = entity.get_mut<NPCContext>();
  if (ctx && !ctx->history.empty() && ctx->history.back().role == "assistant" &&
      !ctx->history.back().content.empty() &&
      ctx->history.back().content.back() != '\n') {
    recordContext("assistant", "\n", true);
  }
}

void AgentBrain::recordContext(const std::string &role, const std::string &text,
                               bool continueTurn) {
  if (text.empty()) return;
  auto ctx = entity.get_mut<NPCContext>();
  if (!ctx) return;
  bool isNewMessage = true;
  if (continueTurn && !ctx->history.empty() && ctx->history.back().role == role) {
    ctx->history.back().content += text;
    isNewMessage = false;
  } else {
    ctx->history.push_back({role, text});
  }

  // A new assistant turn is prefixed so the transcript reads as a dialogue.
  writeTranscript(isNewMessage && role == "assistant" ? "You: " + text : text);
}

void AgentBrain::logNote(const std::string &text) {
  // Deliberately does not touch NPCContext::history: this is for bookkeeping we
  // want in the transcript but must not wake the model with. Timed harvests use
  // it, because they already report back when they complete.
  writeTranscript(text);
}

void AgentBrain::logWarning(const std::string &text) const {
  if (debugLog) {
    debugLog->LogWarning(text);
  }
}

void AgentBrain::writeTranscript(const std::string &text) {
  if (text.empty()) return;
  std::ofstream out(logFilePath, std::ios::app);
  if (out.is_open()) {
    out << text;
  }
}

AI::StreamCallback AgentBrain::getStreamCallback() {
  return [this](const std::string &token) { appendContext("assistant", token); };
}

void AgentBrain::interruptCurrentAction() {
  if (currentAction) {
    currentAction->handleInterruption(entity);
    actionStack.push_back(std::move(currentAction));
    currentAction = nullptr;
  }
}

void AgentBrain::resumeAction() {
  if (!actionStack.empty()) {
    if (currentAction) {
      interruptCurrentAction();
    }
    currentAction = std::move(actionStack.back());
    actionStack.pop_back();
    currentAction->resume(entity);
  } else {
    appendContext("user", "System: There is no interrupted action to resume.\n");
  }
}

void AgentBrain::executeActionQueue(float deltaTime) {
  if (!currentAction) {
    if (!actionStack.empty()) {
      resumeAction();
    } else if (!action_queue.empty()) {
      currentAction = action_queue.front()(entity);
      action_queue.pop_front();
    } else {
      return;
    }
  }

  switch (currentAction->update(deltaTime, entity)) {
  case (ActionStatus::Done): {
    appendContext("user", currentAction->getSuccessMessage());
    currentAction.reset();
    break;
  }
  case (ActionStatus::Doing): {
    break;
  }
  case (ActionStatus::Failed): {
    appendContext("user", currentAction->getFailureMessage());
    currentAction.reset();
    break;
  }
  default:
    currentAction.reset();
    break;
  }
}

void AgentBrain::injectNextAction(ActionThunk actionThunk) {
  if (action_queue.empty()) {
    action_queue.push_back(std::move(actionThunk));
  } else {
    action_queue.insert(action_queue.begin(), std::move(actionThunk));
  }
}

void AgentBrain::addCmdToQueue(ActionThunk actionThunk) {
  action_queue.push_back(std::move(actionThunk));
}

void AgentBrain::update(float deltaTime) {
  if (entity.get<AgentSleepTimer>()) {
    AgentSleepTimer *timer = entity.get_mut<AgentSleepTimer>();
    timer->time_remaining_ms -= deltaTime;
    if (timer->time_remaining_ms <= 0) {
      entity.world().defer([&] { entity.remove<AgentSleepTimer>(); });
    }
    return;
  }

  if (currentAction || !action_queue.empty()) {
    executeActionQueue(deltaTime);
    entity.set<AgentSleepTimer>({10});
    return;
  }

  if (isStopped) return;

  const AIRequest *request = entity.get<AIRequest>();
  const std::string context = getContext();
  if (request == nullptr) {
    awaitingRetry = false;
    auto ctx = entity.get<NPCContext>();
    if (ctx && ctx->history.size() > 1) {
      entity.set<AIRequest>({"System: What is your next command?\n", false, "", false});
    } else {
      entity.set<AIRequest>({"System: Based on your background and current location, what is your first command?\n", false, "", false});
    }
    entity.set<AgentSleepTimer>({10});
    return;
  }
  if (!request->finished) {
    entity.set<AgentSleepTimer>({10});
    return;
  }

  std::string responseText = request->pendingResponse;

  // An empty response is never a command. It means the provider rejected the
  // request or the stream died, so flag it in the debug log and retry. Parsing
  // it as a command would only append an "invalid command" turn and grow the
  // context for no reason.
  if (responseText.empty()) {
    if (!awaitingRetry) {
      awaitingRetry = true;
      if (debugLog) {
        std::string who = "NPC";
        if (entity.has<DisplayName>()) {
          who = entity.get<DisplayName>()->name;
        } else if (entity.name().c_str() != nullptr) {
          who = entity.name().c_str();
        }
        debugLog->LogWarning("Empty AI response for " + who + " - retrying");
      }
      entity.set<AgentSleepTimer>({DEFAULT_EMPTY_RESPONSE_RETRY_MS});
      return;
    }

    // Backoff elapsed: re-arm the same request. Clearing the prompt stops the
    // dispatcher from appending a duplicate turn to the history.
    awaitingRetry = false;
    if (AIRequest *mutableRequest = entity.get_mut<AIRequest>()) {
      mutableRequest->prompt.clear();
      mutableRequest->pendingResponse.clear();
      mutableRequest->finished = false;
      mutableRequest->dispatched = false;
    }
    entity.set<AgentSleepTimer>({10});
    return;
  }

  awaitingRetry = false;

  ActionThunk actionThunk = ParseMessageCommand(responseText);
  
  addCmdToQueue(actionThunk);

  entity.remove<AIRequest>();
}
