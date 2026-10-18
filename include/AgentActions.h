#pragma once

#include "Components.h"
#include "DebugLog.h"
#include "Map.h"
#include "NavigationPlan.hpp"
#include "PlaceQuery.hpp"
#include "Settings.h"
#include "SightReport.hpp"
#include "PathFinding.h"
#include "Defaults.h"
#include "EquipmentRuntime.hpp"
#include "ObjectFactory.h"
#include "InteractionRegistry.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <flecs.h>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stop_token>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>
#include "StringUtils.hpp"
#include "GlobalCommandRegistry.h"
#include "GuideBuilder.h"
#include "TaskKindRegistry.h"
#include "TaskList.hpp"

// Comma-separated list of names, for the "pick one of these" index and failure
// messages the guide commands answer with.
inline std::string FormatNameList(const std::vector<std::string> &names) {
  std::string out;
  for (const std::string &name : names) {
    if (!out.empty()) out += ", ";
    out += name;
  }
  return out;
}

enum class ActionStatus { Doing, Done, Failed, Interrupted };

// Appended to every "that did not work" reply, so an agent whose verb was
// unrecognized or unavailable for the thing in front of it is pointed at the
// command that explains the vocabulary. Both forms are named because they answer
// different questions: the whole list, and one verb in full. Kept without a
// leading space so each call site decides the separator it needs.
inline const char *const HELP_HINT =
    "Use [HELP] to list every command with a short description, or "
    "[HELP <COMMAND>] for a full explanation, e.g. [HELP MOVE_TO].";

class AgentAction {
public:
  virtual ~AgentAction() = default;

  // Returns true when the action is completely finished
  virtual ActionStatus update(float deltaTime, flecs::entity entity) = 0;
  virtual ActionStatus handleInterruption(flecs::entity entity) = 0;
  virtual void resume(flecs::entity entity) = 0;
  virtual std::string getSuccessMessage() = 0;
  virtual std::string getFailureMessage() {
          return "System: Your action has failed. What's next?\n";
        }

  // Tag-style name of the action (e.g. "MOVE_TO", "HARVEST"). Shown by the debug UI.
  virtual std::string getActionName() const { return "ACTION"; }

  // What the action acts on (e.g. the destination of a MOVE_TO, the partner of
  // a TALK_TO), shown next to the name by the debug UI. Empty when the action
  // has no interesting subject. Called once per frame while the NPC menu is
  // open, so keep it cheap and side-effect free.
  virtual std::string getActionContext() const { return ""; }
      };

      class InvalidAction : public AgentAction {
      public:
        InvalidAction() : time(2.0f * 1000.0f) {};

        std::string getActionName() const override { return "INVALID"; }

        ActionStatus update(float deltaTime, flecs::entity) override {
          time -= deltaTime;
          if (time <= 0.0f) {
            return ActionStatus::Done;
          }
          return ActionStatus::Doing;
        }

        std::string getSuccessMessage() override {
          std::string msg = "System: Your previous action was invalid or unrecognized. Please remember to use one of the available commands: ";
          bool first = true;
          for (const auto& cmd : GlobalCommandRegistry::GetCommands()) {
            // Hidden verbs (the trade commands, [PLACE], [GENERIC_INTERACT])
            // are surfaced contextually rather than advertised here.
            if (cmd.hidden || cmd.format.empty()) continue;
            
            if (!first) msg += ", ";
            msg += cmd.format;
            first = false;
          }
          msg += ".\n";
          // The list above is names only; HELP is where an agent can read what a
          // verb it has forgotten actually does.
          msg += HELP_HINT;
          msg += "\n";
          return msg;
        }

        ActionStatus handleInterruption(flecs::entity) override {
          return ActionStatus::Interrupted;
        }

        void resume(flecs::entity) override {}

      private:
        float time;
      };

      class WaitAction : public AgentAction {
      public:
        // time is passed in as seconds, but our deltaTime is in milliseconds
        WaitAction(float time_seconds) : time(time_seconds * 1000.0f) {};

        std::string getActionName() const override { return "WAIT"; }

        // The only interesting thing about a wait is how much of it is left.
        // `time` is milliseconds, see the constructor.
        std::string getActionContext() const override {
          char buf[32];
          std::snprintf(buf, sizeof(buf), "%.1fs left", time / 1000.0f);
          return buf;
        }

        ActionStatus update(float deltaTime, flecs::entity) override {
          time -= deltaTime;
          if (time <= 0.0f) {
            return ActionStatus::Done;
          }
          return ActionStatus::Doing;
        }

        std::string getSuccessMessage() override {
          return "System: You have awaited for a while, what's next?\n";
        }

        ActionStatus handleInterruption(flecs::entity) override {
          return ActionStatus::Interrupted;
        }

        void resume(flecs::entity) override {}

      private:
        float time;
      };

      class MoveToEntityAction : public AgentAction {
      public:
        MoveToEntityAction(std::string targetName) : targetName(targetName), lastKnownTargetPos({-1, -1}) {};

        std::string getActionName() const override { return "MOVE_TO"; }

        std::string getActionContext() const override {
          // Prefer the resolved entity's name so the label shows the canonical
          // spelling. Locations have no entity, so fall back to the request.
          if (targetEntity.is_alive() && targetEntity.has<DisplayName>()) {
            return targetEntity.get<DisplayName>()->name;
          }
          return targetName;
        }

        ActionStatus update(float, flecs::entity entity) override {
          Map *map = entity.world().get<MapResource>()->map;
          const GamePosition startPos = *entity.get<GamePosition>();

          GamePosition targetPos = {-1, -1};
          flecs::entity resolvedTarget = flecs::entity::null();
          bool found = false;

          entity.world().filter<DisplayName, GamePosition>().each(
            [&](flecs::entity other, const DisplayName &name, const GamePosition &pos) {
              if (StringUtils::EqualsIgnoreCase(name.name, targetName)) {
                targetPos = pos;
                resolvedTarget = other;
                found = true;
              }
          });

          // Not an entity: try the places this character remembers. A place only
          // resolves if it has been observed, which is what makes a name an
          // observation rather than a coordinate the map handed over.
          if (!found) {
            if (const SpatialMemory *memory = entity.get<SpatialMemory>()) {
              // A label may name more than one place. A road cut at junctions is
              // several stretches, and an agent naming them all "road" should get
              // the one it can act on, not whichever happens to have the lowest
              // id: choose the place whose approach cell is nearest. With the
              // far-end rule for linear places, that is the stretch ahead.
              const KnownPlace *best = nullptr;
              GamePosition bestCell = {-1, -1};
              int bestDistance = -1;
              for (const KnownPlace &place : memory->places) {
                bool matches = false;
                for (const std::string &label : place.labels) {
                  if (StringUtils::EqualsIgnoreCase(label, targetName)) {
                    matches = true;
                    break;
                  }
                }
                if (!matches) continue;

                const GamePosition cell =
                    PlaceQuery::ApproachCell(*memory, place, startPos);
                const int distance = std::max(std::abs(cell.x - startPos.x),
                                              std::abs(cell.y - startPos.y));
                if (best == nullptr || distance < bestDistance) {
                  best = &place;
                  bestCell = cell;
                  bestDistance = distance;
                }
              }
              if (best != nullptr) {
                targetPos = bestCell;
                found = true;
              }
            }
          }

          // A location is not an entity, so this stays null for those.
          targetEntity = resolvedTarget;

          if (!found) {
            entity.remove<MOVE_THROUGH_PATH_ACTION>();
            return ActionStatus::Failed;
          }

          if (startPos == targetPos || Map::AreNeighbours(startPos, targetPos)) {
            entity.remove<MOVE_THROUGH_PATH_ACTION>();
            // Objects expose their verbs through InteractionTarget so a
            // follow-up [STORE] / [HARVEST] resolves against this target.
            // Characters are talked to, so they are deliberately excluded.
            if (targetEntity.is_alive() && !targetEntity.has<CharacterTag>()) {
              entity.set<InteractionTarget>({targetEntity});
            }
            return ActionStatus::Done;
          } 
          
          // If we don't have a path, OR the target has moved since we last calculated the path:
          if (!entity.has<MOVE_THROUGH_PATH_ACTION>() || !(lastKnownTargetPos == targetPos)) {
            entity.set<MOVE_THROUGH_PATH_ACTION>({AStar(map, startPos, targetPos)});
            lastKnownTargetPos = targetPos;
          }

          return ActionStatus::Doing;
        }

        std::string getSuccessMessage() override {
          std::string message =
              "System: You have arrived next to " + targetName + ".\n";

          if (!targetEntity.is_alive() || targetEntity.has<CharacterTag>()) {
            return message;
          }

          auto interactions =
              InteractionRegistry::GetAvailableInteractions(targetEntity);
          if (interactions.empty()) {
            return message;
          }

          message += "You can interact with the " + targetName + " using:\n";
          for (const auto& interaction : interactions) {
            message += FormatInteractionForAI(StringUtils::ToUpper(interaction.name),
                                              interaction.description);
          }
          return message;
        }

        std::string getFailureMessage() override {
          return "System: Could not find character or location " + targetName + " to move to. What's next?\n";
        }

        ActionStatus handleInterruption(flecs::entity entity) override {
          entity.remove<MOVE_THROUGH_PATH_ACTION>();
          return ActionStatus::Interrupted;
        }

        void resume(flecs::entity) override {}

      private:
        std::string targetName;
        GamePosition lastKnownTargetPos;
        // Resolved while pathing so the arrival message can list this object's
        // interactions, and so InteractionTarget can point at it.
        flecs::entity targetEntity = flecs::entity::null();
      };

      // Turns on the spot and reports what is now in sight.
      //
      // `Facing` drives the vision cone, and an NPC's facing is derived from its
      // velocity -- so its cone is locked to its direction of travel. At a
      // junction or at the end of a road that means it cannot look along the
      // other arm without walking onto it, which is exactly where this verb
      // matters (docs/agent-exploration-task.md gap 5).
      //
      // The action sets `Facing`, waits one frame for the sampler to take the new
      // sweep (the sampler is registered after the action systems, and `Vision`'s
      // cache is sampler-owned), then renders the sweep through SightReport. A
      // turn below the resample threshold keeps the existing sweep -- it is still
      // the sweep for this facing -- so that case answers immediately.
      //
      // `[LOOK around]` is the same verb with a one-shot full-circle sample: the
      // sampler sweeps 360 degrees once and clears the request, so the character's
      // own cone is never edited. It reports all eight sectors, which is what
      // "where am I" actually asks. The cells, objects and places enter memory
      // through the normal writer because they are just a sample like any other.
      class LookAction : public AgentAction {
      public:
        explicit LookAction(std::string direction)
            : direction(std::move(direction)) {}

        std::string getActionName() const override { return "LOOK"; }

        std::string getActionContext() const override { return direction; }

        ActionStatus update(float, flecs::entity entity) override {
          if (phase == Phase::Waiting) {
            const Vision *vision = entity.get<Vision>();
            const int sampleTick =
                vision == nullptr
                    ? requestedTick
                    : (survey ? vision->lastFullCircleTick
                              : vision->lastSampleTick);
            if (sampleTick == requestedTick) {
              if (++waitFrames > kMaxWaitFrames) {
                // Safety net only: with a map and a Facing the sampler always
                // runs. Better a failure the agent can react to than a silent
                // claim that a circle happened when it did not.
                if (survey) {
                  if (Vision *mutableVision = entity.get_mut<Vision>()) {
                    mutableVision->fullCircleRequested = false;
                  }
                }
                response = "System: You cannot make out anything around you.\n";
                return ActionStatus::Failed;
              }
              return ActionStatus::Doing;
            }
            return FinishTurn(entity);
          }

          Vision *vision = entity.get_mut<Vision>();
          Facing *facing = entity.get_mut<Facing>();
          if (vision == nullptr || facing == nullptr) {
            response = "System: You have no sense of direction to turn.\n";
            return ActionStatus::Failed;
          }

          if (IsAround(direction)) {
            survey = true;
            // One-shot: the sampler consumes this and clears it.
            vision->fullCircleRequested = true;
            requestedTick = vision->lastFullCircleTick;
            waitFrames = 0;
            phase = Phase::Waiting;
            return ActionStatus::Doing;  // a full circle needs the sweep
          }

          int dx = 0;
          int dy = 0;
          if (!ParseDirection(direction, dx, dy)) {
            response =
                "System: '" + direction +
                "' is not a direction. Use north, south, east, west, a diagonal "
                "such as north-east, or around.\n";
            return ActionStatus::Failed;
          }

          const raylib::Vector2 next =
              raylib::Vector2(static_cast<float>(dx), static_cast<float>(dy))
                  .Normalize();
          // Before the assignment: this is the turn the sampler will see.
          const float dot = facing->x * next.x + facing->y * next.y;
          const bool eagerSweep = vision->fullCircleRequested;
          *facing = Facing(next);
          heading = DirectionWord(dx, dy);
          requestedTick = vision->lastSampleTick;
          waitFrames = 0;
          phase = Phase::Waiting;

          // A turn under the resample threshold leaves the sampler's sweep as the
          // one that still describes this facing. A pending 360-degree request
          // covers every direction, so it is also worth waiting for.
          const bool resamples =
              eagerSweep ||
              (vision->angleDegrees < 359.5f &&
               dot < std::cos(DEFAULT_VISION_FACING_RESAMPLE_DEGREES * DEG2RAD));
          if (!resamples) {
            return FinishTurn(entity);
          }
          return ActionStatus::Doing;
        }

        std::string getSuccessMessage() override { return response; }

        std::string getFailureMessage() override { return response; }

        ActionStatus handleInterruption(flecs::entity entity) override {
          if (survey) {
            // The sweep may or may not have been consumed. Start the survey over
            // on resume rather than leaving a request nothing will report.
            if (Vision *vision = entity.get_mut<Vision>()) {
              vision->fullCircleRequested = false;
            }
            phase = Phase::Turn;
            requestedTick = -1;
          }
          return ActionStatus::Interrupted;
        }

        void resume(flecs::entity) override {}

      private:
        enum class Phase { Turn, Waiting };

        static constexpr int kMaxWaitFrames = 8;

        ActionStatus FinishTurn(flecs::entity entity) {
          const Vision *vision = entity.get<Vision>();
          const SpatialMemory *memory = entity.get<SpatialMemory>();
          const GamePosition *here = entity.get<GamePosition>();
          const Facing *facing = entity.get<Facing>();
          if (vision == nullptr || memory == nullptr || here == nullptr) {
            response = "System: You cannot make out anything around you.\n";
            return ActionStatus::Failed;
          }

          const std::string facingWord =
              facing != nullptr ? SightReport::FacingWord(*facing) : std::string();
          // A directional look reads the cone's own sweep; a survey reads the
          // copy the sampler kept for it, because the cone may already have been
          // restored by the time this action is stepped again.
          const std::vector<GamePosition> &visible =
              survey ? vision->lastFullCircle : vision->lastVisible;
          const int visibleTick =
              survey ? vision->lastFullCircleTick : vision->lastSampleTick;
          response = SightReport::Render(*memory, visible, *here, visibleTick,
                                         facingWord, survey, heading);
          return ActionStatus::Done;
        }

        // "around", "all", "360" and "everywhere" mean the full circle. Checked
        // before ParseDirection so the direction parser stays about directions.
        static bool IsAround(const std::string &raw) {
          std::string text;
          for (char c : raw) {
            if (c == ' ' || c == '-' || c == '_') continue;
            text += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
          }
          return text == "around" || text == "all" || text == "allaround" ||
                 text == "360" || text == "everywhere";
        }

        // "north-east", "northeast", "NE" and "ne" all mean the same thing; case,
        // spaces, hyphens and underscores are ignored. Single letters are
        // accepted because a model that has already understood the convention
        // should not have its shorthand rejected.
        static bool ParseDirection(const std::string &raw, int &dx, int &dy) {
          std::string text;
          for (char c : raw) {
            if (c == ' ' || c == '-' || c == '_') continue;
            text += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
          }
          if (text == "n" || text == "north") { dx = 0; dy = -1; }
          else if (text == "s" || text == "south") { dx = 0; dy = 1; }
          else if (text == "e" || text == "east") { dx = 1; dy = 0; }
          else if (text == "w" || text == "west") { dx = -1; dy = 0; }
          else if (text == "ne" || text == "northeast") { dx = 1; dy = -1; }
          else if (text == "se" || text == "southeast") { dx = 1; dy = 1; }
          else if (text == "sw" || text == "southwest") { dx = -1; dy = 1; }
          else if (text == "nw" || text == "northwest") { dx = -1; dy = -1; }
          else return false;
          return true;
        }

        static std::string DirectionWord(int dx, int dy) {
          if (dy < 0) return dx == 0 ? "north" : (dx > 0 ? "north-east" : "north-west");
          if (dy > 0) return dx == 0 ? "south" : (dx > 0 ? "south-east" : "south-west");
          return dx > 0 ? "east" : "west";
        }

        std::string direction;
        std::string response;
        std::string heading;
        bool survey = false;
        Phase phase = Phase::Turn;
        int requestedTick = -1;
        int waitFrames = 0;
      };

      // A movement leg whose target is a place id, not a name.
      //
      // [MOVE_TO] resolves names, and an inferred place may have none; navigation
      // needs to walk to the stretch System 1 chose even when the character has
      // never named it. Same path/arrival discipline as MoveToEntityAction, minus
      // the name lookup and the interaction listing -- a place is ground, not an
      // object.
      class MoveToPlaceAction : public AgentAction {
      public:
        explicit MoveToPlaceAction(int placeId) : placeId(placeId) {}

        std::string getActionName() const override { return "MOVE_TO_PLACE"; }

        std::string getActionContext() const override { return placeName; }

        ActionStatus update(float, flecs::entity entity) override {
          const SpatialMemory *memory = entity.get<SpatialMemory>();
          const GamePosition *here = entity.get<GamePosition>();
          const MapResource *mapRes = entity.world().get<MapResource>();
          if (memory == nullptr || here == nullptr || mapRes == nullptr ||
              mapRes->map == nullptr) {
            failureReason = "you cannot find your way right now";
            return ActionStatus::Failed;
          }

          const KnownPlace *place = PlaceQuery::ById(*memory, placeId);
          if (place == nullptr) {
            failureReason = "that place is no longer in your memory";
            return ActionStatus::Failed;
          }
          placeName = PlaceQuery::DisplayName(*place);

          // The target is fixed when the leg begins, not recomputed every frame.
          // For a linear place (a road) ApproachCell returns the far end from the
          // character's current cell, and that far end flips to the other end at
          // the midpoint; recomputing it while walking would turn the character
          // around there. One leg means one destination.
          if (!targetSet) {
            target = PlaceQuery::ApproachCell(*memory, *place, *here);
            targetSet = true;
          }

          if (*here == target || Map::AreNeighbours(*here, target)) {
            entity.remove<MOVE_THROUGH_PATH_ACTION>();
            return ActionStatus::Done;
          }

          if (!entity.has<MOVE_THROUGH_PATH_ACTION>()) {
            const std::vector<GamePosition> path =
                AStar(mapRes->map, *here, target);
            if (path.empty()) {
              entity.remove<MOVE_THROUGH_PATH_ACTION>();
              failureReason =
                  "there is no way to reach " + placeName + " from here";
              return ActionStatus::Failed;
            }
            entity.set<MOVE_THROUGH_PATH_ACTION>({path});
          }
          return ActionStatus::Doing;
        }

        std::string getSuccessMessage() override {
          return "System: You have arrived at " + placeName + ".\n";
        }

        std::string getFailureMessage() override {
          return "System: You could not walk there: " + failureReason + ".\n";
        }

        // Without the "System:" wrapper, for a caller that folds it into its own
        // summary. Navigation does, so the summary reads as one message.
        const std::string &getFailureReason() const { return failureReason; }

        ActionStatus handleInterruption(flecs::entity entity) override {
          entity.remove<MOVE_THROUGH_PATH_ACTION>();
          return ActionStatus::Interrupted;
        }

        void resume(flecs::entity) override {}

      private:
        int placeId;
        std::string placeName;
        std::string failureReason;
        GamePosition target{-1, -1};
        bool targetSet = false;
      };

      // System 1 navigation: ask which remembered place is the best next step,
      // walk there, and decide again on arrival (stage 2 of
      // docs/navigation-subagent-design.md).
      //
      // The main model states a goal once. The action asks the decision model,
      // walks one leg, and re-asks with the updated memory; it stops on `arrived`,
      // `none`, a budget, no progress, or a survey that failed to settle the
      // choice, and reports once. Interruption preserves the episode so a wake
      // can read a sign or greet someone and then resume.
      //
      // Two things are deliberately not here:
      //   * the candidate set is built by NavigationPlan from the character's own
      //     SpatialMemory, never from the map;
      //   * the answer is resolved by place id, so a hallucinated name cannot
      //     move the character.
      class NavigateAction : public AgentAction {
      public:
        explicit NavigateAction(std::string goal)
            : goal(SanitiseGoal(std::move(goal))) {}

        ~NavigateAction() override { AbandonPending(); }

        std::string getActionName() const override { return "NAVIGATE"; }

        std::string getActionContext() const override {
          return legs > 0
                     ? goal + " (leg " + std::to_string(legs + 1) + ")"
                     : goal;
        }

        ActionStatus update(float deltaTime, flecs::entity entity) override {
          const GameSettings::SystemOneNavigationSection &nav =
              Settings().system1.navigation;
          if (!nav.enabled) {
            return Finish(entity, "navigation is turned off in the settings");
          }
          if (!started) {
            started = true;
            startedAt = Clock::now();
          }
          if (SecondsSince(startedAt) > nav.maxSeconds) {
            return Finish(entity, "you reached the time limit without arriving");
          }

          switch (phase) {
            case Phase::Idle:
              return StartDecision(entity, nav);
            case Phase::Asking:
              return PollAnswer(entity, nav);
            case Phase::Surveying:
              return UpdateSurvey(entity, nav);
            case Phase::Moving:
              return UpdateLeg(deltaTime, entity, nav);
          }
          return Finish(entity, "your sense of direction failed");
        }

        std::string getSuccessMessage() override { return response; }

        std::string getFailureMessage() override {
          return response.empty() ? "System: Navigation failed.\n" : response;
        }

        ActionStatus handleInterruption(flecs::entity entity) override {
          Trace(entity, std::string("interrupted while ") + PhaseName(phase) +
                            " (legs=" + std::to_string(legs) + ", decisions=" +
                            std::to_string(decisions) + "); episode kept");
          AbandonPending();
          if (leg) {
            leg->handleInterruption(entity);
            leg.reset();
          }
          // The episode survives: goal, visited set, budgets, leg counter and a
          // survey already in flight all stay. An interrupted ask starts over,
          // because its answer was cancelled; a moving leg is recreated on
          // resume (the character may have been moved by the wake action).
          if (phase == Phase::Asking) phase = Phase::Idle;
          return ActionStatus::Interrupted;
        }

        void resume(flecs::entity entity) override {
          Trace(entity, std::string("resumed in ") + PhaseName(phase));
        }

      private:
        using Clock = std::chrono::steady_clock;

        enum class Phase { Idle, Asking, Surveying, Moving };

        // The callback may run on the provider's worker thread. It must not
        // capture `this`; this struct is the whole handoff. `abandoned` is the
        // action telling a late callback to drop its answer.
        struct Pending {
          std::mutex mutex;
          bool ready = false;
          bool abandoned = false;
          std::vector<SystemOne::Answer> answers;
          SystemOne::CallTrace trace;
        };

        // About eight action updates: the sampler runs every frame and the brain
        // steps a running action every other frame, so a survey that has not
        // landed by then is not going to.
        static constexpr int kMaxSurveyWaitUpdates = 10;

        static std::string SanitiseGoal(std::string text) {
          // The goal goes into a prompt and into messages. One line, no control
          // characters, no brackets that could look like a command, capped so a
          // model cannot turn the state into an essay.
          std::string cleaned;
          cleaned.reserve(text.size());
          bool lastSpace = false;
          for (char c : text) {
            const unsigned char uc = static_cast<unsigned char>(c);
            if (c == '[' || c == ']' || std::iscntrl(uc) != 0) {
              c = ' ';
            }
            if (c == ' ') {
              if (lastSpace || cleaned.empty()) continue;
              lastSpace = true;
            } else {
              lastSpace = false;
            }
            cleaned += c;
          }
          while (!cleaned.empty() && cleaned.back() == ' ') cleaned.pop_back();
          constexpr std::size_t kMaxGoalLength = 200;
          if (cleaned.size() > kMaxGoalLength) {
            cleaned.resize(kMaxGoalLength);
            // Do not leave half a UTF-8 sequence behind: the state is serialized
            // as JSON, and an invalid sequence would make the provider's dump
            // throw rather than return a bad decision.
            while (!cleaned.empty() &&
                   (static_cast<unsigned char>(cleaned.back()) & 0xC0) == 0x80) {
              cleaned.pop_back();
            }
            if (!cleaned.empty()) {
              const unsigned char lead =
                  static_cast<unsigned char>(cleaned.back());
              if ((lead & 0x80) != 0 && (lead & 0xE0) == 0xC0) {
                cleaned.pop_back();
              } else if ((lead & 0xF0) == 0xE0) {
                cleaned.pop_back();
              } else if ((lead & 0xF8) == 0xF0) {
                cleaned.pop_back();
              }
            }
            while (!cleaned.empty() && cleaned.back() == ' ') cleaned.pop_back();
          }
          return cleaned;
        }

        static std::string AgentName(flecs::entity entity) {
          if (const DisplayName *name = entity.get<DisplayName>()) {
            if (!name->name.empty()) return name->name;
          }
          const std::string named = entity.name().c_str();
          return named.empty() ? std::string("character") : named;
        }

        static int Chebyshev(const GamePosition &a, const GamePosition &b) {
          return std::max(std::abs(a.x - b.x), std::abs(a.y - b.y));
        }

        static double SecondsSince(const Clock::time_point &when) {
          return std::chrono::duration<double>(Clock::now() - when).count();
        }

        static std::string FormatScore(float value) {
          if (value < 0.0f) return "n/a";
          std::ostringstream out;
          out.setf(std::ios::fixed);
          out.precision(2);
          out << value;
          return out.str();
        }

        static const char *KindName(NavigationPlan::ChoiceOutcome::Kind kind) {
          using Kind = NavigationPlan::ChoiceOutcome::Kind;
          switch (kind) {
            case Kind::Place: return "place";
            case Kind::Arrived: return "arrived";
            case Kind::None: return "none";
            case Kind::Unsure: return "unsure";
            case Kind::Invalid: return "invalid";
          }
          return "?";
        }

        static const char *PhaseName(Phase phase) {
          switch (phase) {
            case Phase::Idle: return "idle";
            case Phase::Asking: return "asking";
            case Phase::Surveying: return "surveying";
            case Phase::Moving: return "moving";
          }
          return "?";
        }

        // One line per navigation event, in the debug log. Stage 3 tunes from
        // these plus the System 1 records; the recorder has the full payload, this
        // has the sequence in order, which is what a run needs when the question
        // is "did it look, choose, walk, and why did it stop".
        void Trace(flecs::entity entity, const std::string &text) {
          const DebugLogResource *resource =
              entity.world().get<DebugLogResource>();
          if (resource == nullptr || resource->log == nullptr) return;
          resource->log->LogInfo("[navigation] " + AgentName(entity) + " " +
                                 text);
        }

        void AbandonPending() {
          stopSource.request_stop();
          if (pending) {
            std::lock_guard<std::mutex> lock(pending->mutex);
            pending->abandoned = true;
          }
          pending.reset();
        }

        // Build the summary and stop. Any live leg is cancelled first: an episode
        // that ends on the clock must not leave the character walking.
        ActionStatus Finish(flecs::entity entity, const std::string &reason) {
          Trace(entity, "finished: " + reason + " (legs=" + std::to_string(legs) +
                            ", decisions=" + std::to_string(decisions) +
                            ", surveys=" + std::to_string(surveysUsed) + ")");
          if (leg) {
            leg->handleInterruption(entity);
            leg.reset();
          }
          if (phase == Phase::Surveying) {
            // Do not leave a sweep requested by an episode that just ended.
            if (Vision *vision = entity.get_mut<Vision>()) {
              vision->fullCircleRequested = false;
            }
          }
          const SpatialMemory *memory = entity.get<SpatialMemory>();
          const GamePosition *here = entity.get<GamePosition>();
          if (memory != nullptr && here != nullptr) {
            response = NavigationPlan::BuildSummary(reason, route, *memory,
                                                    *here, legs);
          } else {
            response = "System: You stopped: " + reason + ".\n";
          }
          return ActionStatus::Done;
        }

        bool CanSurvey(flecs::entity entity,
                       const GameSettings::SystemOneNavigationSection &nav) {
          return !surveyedThisDecision && surveysUsed < nav.maxSurveys &&
                 entity.get<Vision>() != nullptr;
        }

        ActionStatus StartSurvey(flecs::entity entity) {
          Vision *vision = entity.get_mut<Vision>();
          if (vision == nullptr) {
            return Finish(entity, "you cannot look around");
          }
          vision->fullCircleRequested = true;
          surveyRequestedTick = vision->lastFullCircleTick;
          surveyWaitUpdates = 0;
          surveyedThisDecision = true;
          ++surveysUsed;
          Trace(entity, "step " + std::to_string(legs) +
                            ": unsure, full-circle survey #" +
                            std::to_string(surveysUsed));
          phase = Phase::Surveying;
          return ActionStatus::Doing;
        }

        ActionStatus UpdateSurvey(
            flecs::entity entity,
            const GameSettings::SystemOneNavigationSection &) {
          Vision *vision = entity.get_mut<Vision>();
          if (vision == nullptr) {
            return Finish(entity, "you cannot look around");
          }
          if (vision->lastFullCircleTick != surveyRequestedTick) {
            // The sweep happened. The writer and deriver ran in the same frame,
            // and the action is stepped a frame later, so the next candidate
            // build sees the new places.
            afterSurvey = true;
            Trace(entity, "full circle complete; re-asking");
            phase = Phase::Idle;
            return ActionStatus::Doing;
          }
          if (++surveyWaitUpdates > kMaxSurveyWaitUpdates) {
            vision->fullCircleRequested = false;
            Trace(entity, "full circle did not complete");
            return Finish(entity, "you could not look around");
          }
          return ActionStatus::Doing;
        }

        ActionStatus StartDecision(
            flecs::entity entity,
            const GameSettings::SystemOneNavigationSection &nav) {
          const SpatialMemory *memory = entity.get<SpatialMemory>();
          const GamePosition *here = entity.get<GamePosition>();
          if (memory == nullptr || here == nullptr) {
            response = "System: You have no memory of where you are.\n";
            return ActionStatus::Failed;
          }

          if (decisions >= nav.maxDecisions) {
            return Finish(entity,
                          "you reached the decision limit without arriving");
          }

          // The start of the route is recorded before the decision: the summary
          // must say where the leg began, not only where it ended.
          if (route.empty()) {
            route.push_back(NavigationPlan::StartPlaceName(*memory, *here));
          }
          // The re-ask after a survey is part of the same decision: it must not
          // open the door to a second survey for that decision.
          if (!afterSurvey) {
            surveyedThisDecision = false;
          }
          afterSurvey = false;

          candidates = NavigationPlan::BuildCandidates(
              *memory, *here, nav.maxCandidates, visitedPlaceIds);
          candidates.erase(
              std::remove_if(
                  candidates.begin(), candidates.end(),
                  [this](const NavigationPlan::Candidate &candidate) {
                    return unreachablePlaceIds.count(candidate.placeId) != 0;
                  }),
              candidates.end());

          if (candidates.empty()) {
            if (CanSurvey(entity, nav)) return StartSurvey(entity);
            return Finish(entity,
                          "you do not remember any place that could lead there");
          }

          const SystemOneClassifierResource *resource =
              entity.world().get<SystemOneClassifierResource>();
          if (resource == nullptr || resource->classifier == nullptr ||
              !resource->classifier->Available()) {
            return Finish(entity, "System 1 is not available");
          }
          classifier = resource->classifier;

          const Facing *facing = entity.get<Facing>();
          const std::string state = NavigationPlan::BuildState(
              *memory, *here, facing != nullptr ? *facing : Facing{},
              AgentName(entity), goal, legs, visitedNames, candidates);
          const SystemOne::Option question =
              NavigationPlan::BuildQuestion(*memory, *here, candidates);

          pending = std::make_shared<Pending>();
          stopSource = std::stop_source();
          ++decisions;
          Trace(entity, "step " + std::to_string(legs) + ": asking (" +
                            std::to_string(candidates.size()) +
                            " candidates, decision " +
                            std::to_string(decisions) + ", floor " +
                            FormatScore(nav.minProbability) + "/" +
                            FormatScore(nav.minMargin) + ") for: " + goal);

          SystemOne::Attribution attribution;
          attribution.scenario = "navigation";
          attribution.summary = AgentName(entity) + ", step " +
                                std::to_string(legs) + " toward: " + goal;
          attribution.advisory = false;

          std::shared_ptr<Pending> shared = pending;
          classifier->Ask(
              state, {question},
              [shared](const std::vector<SystemOne::Answer> &answers,
                       const SystemOne::CallTrace &trace) {
                std::lock_guard<std::mutex> lock(shared->mutex);
                if (shared->abandoned) return;
                shared->answers = answers;
                shared->trace = trace;
                shared->ready = true;
              },
              stopSource.get_token(), attribution);

          phase = Phase::Asking;
          return ActionStatus::Doing;
        }

        // An uncertain answer: one survey for this decision if the budget allows,
        // otherwise hand back with a reason that says whether a survey was tried.
        ActionStatus StopUnsure(
            flecs::entity entity,
            const GameSettings::SystemOneNavigationSection &nav,
            const std::string &withoutSurvey) {
          if (CanSurvey(entity, nav)) {
            return StartSurvey(entity);
          }
          return Finish(entity,
                        surveyedThisDecision
                            ? "you turned a full circle and still could not choose"
                            : withoutSurvey);
        }

        ActionStatus PollAnswer(
            flecs::entity entity,
            const GameSettings::SystemOneNavigationSection &nav) {
          if (!pending) {
            phase = Phase::Idle;
            return ActionStatus::Doing;
          }

          std::vector<SystemOne::Answer> answers;
          SystemOne::CallTrace trace;
          {
            std::lock_guard<std::mutex> lock(pending->mutex);
            if (!pending->ready) return ActionStatus::Doing;
            answers = pending->answers;
            trace = pending->trace;
            pending->ready = false;
          }

          const SpatialMemory *memory = entity.get<SpatialMemory>();
          const GamePosition *here = entity.get<GamePosition>();
          if (memory == nullptr || here == nullptr) {
            response = "System: You have no memory of where you are.\n";
            return ActionStatus::Failed;
          }

          if (!trace.Ok()) {
            return Finish(entity, "System 1 did not answer");
          }

          const SystemOne::Answer *answer = nullptr;
          for (const SystemOne::Answer &candidate : answers) {
            if (candidate.id == "next") {
              answer = &candidate;
              break;
            }
          }
          if (answer == nullptr) {
            return Finish(entity, "the decision could not be read");
          }

          const NavigationPlan::ChoiceOutcome outcome =
              NavigationPlan::InterpretChoice(*answer, candidates,
                                              nav.minProbability,
                                              nav.minMargin);

          Trace(entity,
                "step " + std::to_string(legs) + ": choice '" + answer->choice +
                    "' " + KindName(outcome.kind) + " p=" +
                    FormatScore(outcome.probability) + " margin=" +
                    FormatScore(outcome.margin) +
                    (surveyedThisDecision ? " (after survey)" : ""));

          switch (outcome.kind) {
            case NavigationPlan::ChoiceOutcome::Kind::Arrived:
              return Finish(entity, "you judged that you had arrived");
            case NavigationPlan::ChoiceOutcome::Kind::None:
              return StopUnsure(entity, nav,
                                "none of the remembered places looked suitable");
            case NavigationPlan::ChoiceOutcome::Kind::Unsure:
              return StopUnsure(entity, nav,
                                "you could not tell which way looked better");
            case NavigationPlan::ChoiceOutcome::Kind::Invalid:
              return Finish(entity, "the decision could not be read");
            case NavigationPlan::ChoiceOutcome::Kind::Place:
              break;
          }

          const auto chosen = std::find_if(
              candidates.begin(), candidates.end(),
              [&outcome](const NavigationPlan::Candidate &candidate) {
                return candidate.placeId == outcome.placeId;
              });
          if (chosen == candidates.end()) {
            return Finish(entity, "the decision named a place you do not know");
          }

          // The two no-progress rules from the design: the same place twice in a
          // row, or a one-tile approach while the character has not moved since
          // the previous choice.
          if (outcome.placeId == lastChosenPlaceId) {
            return Finish(entity, "you would have been going back and forth");
          }
          if (lastChosenPlaceId != 0 && chosen->distance <= 1 &&
              Chebyshev(*here, positionAtLastChoice) <= 1) {
            return Finish(entity, "you would have been going back and forth");
          }

          lastChosenPlaceId = outcome.placeId;
          positionAtLastChoice = *here;

          const KnownPlace *place = PlaceQuery::ById(*memory, outcome.placeId);
          legName = place != nullptr ? PlaceQuery::DisplayName(*place)
                                     : chosen->description;
          legPlaceId = outcome.placeId;
          leg = std::make_unique<MoveToPlaceAction>(outcome.placeId);
          legStartedAt = Clock::now();
          phase = Phase::Moving;
          return ActionStatus::Doing;
        }

        ActionStatus HandleLegFailure(flecs::entity entity,
                                      const std::string &reason) {
          if (unreachablePlaceIds.count(legPlaceId) != 0) {
            return Finish(entity, reason);
          }
          unreachablePlaceIds.insert(legPlaceId);
          // One retry: the place is filtered from the next candidate list, so
          // this cannot loop on the same target.
          phase = Phase::Idle;
          return ActionStatus::Doing;
        }

        ActionStatus UpdateLeg(
            float deltaTime, flecs::entity entity,
            const GameSettings::SystemOneNavigationSection &nav) {
          const SpatialMemory *memory = entity.get<SpatialMemory>();
          const GamePosition *here = entity.get<GamePosition>();
          if (memory == nullptr || here == nullptr) {
            response = "System: You have no memory of where you are.\n";
            return ActionStatus::Failed;
          }

          if (!leg) {
            // Resumed after a wake, or the first update after the decision.
            leg = std::make_unique<MoveToPlaceAction>(legPlaceId);
            legStartedAt = Clock::now();
          }

          if (SecondsSince(legStartedAt) > nav.legTimeoutSeconds) {
            leg->handleInterruption(entity);
            leg.reset();
            return HandleLegFailure(
                entity, "you could not reach " + legName + " in time");
          }

          const ActionStatus status = leg->update(deltaTime, entity);
          if (status == ActionStatus::Doing) return ActionStatus::Doing;

          if (status == ActionStatus::Done) {
            ++legs;
            visitedPlaceIds.insert(legPlaceId);
            visitedNames.push_back(legName);
            route.push_back(legName);
            Trace(entity, "leg " + std::to_string(legs) + ": arrived at " +
                              legName);
            leg.reset();
            // The next decision. The episode ends only through the stop
            // conditions, never because one leg completed.
            phase = Phase::Idle;
            return ActionStatus::Doing;
          }

          const std::string reason = leg->getFailureReason();
          Trace(entity, "leg " + std::to_string(legs + 1) + ": failed (" +
                            reason + ") for " + legName);
          leg.reset();
          return HandleLegFailure(entity, reason);
        }

        std::string goal;
        std::string response;
        Phase phase = Phase::Idle;
        std::shared_ptr<SystemOne::SystemOneClassifier> classifier;
        std::shared_ptr<Pending> pending;
        std::stop_source stopSource;
        std::vector<NavigationPlan::Candidate> candidates;
        std::vector<std::string> route;
        std::vector<std::string> visitedNames;
        std::unique_ptr<MoveToPlaceAction> leg;
        std::string legName;
        int legPlaceId = 0;
        int legs = 0;
        int decisions = 0;
        int surveysUsed = 0;
        int surveyWaitUpdates = 0;
        int surveyRequestedTick = -1;
        int lastChosenPlaceId = 0;
        GamePosition positionAtLastChoice{0, 0};
        bool surveyedThisDecision = false;
        bool afterSurvey = false;
        bool started = false;
        Clock::time_point startedAt;
        Clock::time_point legStartedAt;
        std::unordered_set<int> visitedPlaceIds;
        std::unordered_set<int> unreachablePlaceIds;
      };


      enum class ConversationState {
        Talking,
        Listening,
        Ended
      };

      class TalkAction : public AgentAction {
      public:
        TalkAction(flecs::entity sourceEntity, std::string targetName, ConversationState state = ConversationState::Talking);

        std::string getActionName() const override { return "TALK_TO"; }

        std::string getActionContext() const override;

        ActionStatus update(float deltaTime, flecs::entity entity) override;
        ActionStatus handleInterruption(flecs::entity entity) override;
        void resume(flecs::entity entity) override;
        std::string getSuccessMessage() override;
        std::string getFailureMessage() override;

        ConversationState state;
      private:
        std::string targetName;
        flecs::entity targetEntity;
        bool isTalkingToSelf = false;
        // Backoff in milliseconds after an empty response, so a failing provider
        // is not asked again once per frame.
        float retryCooldownMs = 0.0f;
        // Consecutive free-look [INVENTORY] calls within the current turn. Reset
        // whenever the turn ends, so the cap only bounds one agent stalling.
        int consecutivePeeks = 0;
      };

      // A trade verb emitted outside a conversation has nothing to act on. This
      // exists purely to tell the model to start a conversation, instead of
      // handing it the generic "invalid command" reply.
      class TradeOutOfContextAction : public AgentAction {
      public:
        explicit TradeOutOfContextAction(std::string verbName)
            : verb(verbName) {}

        std::string getActionName() const override { return verb; }

        // The verb alone does not explain why this action exists: it is always
        // the "you are not in a conversation" reply to a trade verb.
        std::string getActionContext() const override {
          return "outside a conversation";
        }

        ActionStatus update(float, flecs::entity) override {
          return ActionStatus::Done;
        }

        std::string getSuccessMessage() override {
          return "System: [" + verb +
                 "] only works while you are in a conversation with someone. "
                 "Use [TALK_TO $TARGET] to start talking, then use the trade "
                 "commands inside the conversation.\n";
        }

        ActionStatus handleInterruption(flecs::entity) override {
          return ActionStatus::Interrupted;
        }

        void resume(flecs::entity) override {}

      private:
        std::string verb;
      };

      class CharactersAction : public AgentAction {
      public:
        std::string getActionName() const override { return "CHARACTERS"; }

        ActionStatus update(float, flecs::entity entity) override {
          std::string characterNames;
          auto name_filter = entity.world().filter<DisplayName, CharacterTag>();
          
          const DisplayName* myNameComp = entity.get<DisplayName>();
          std::string myName = myNameComp ? myNameComp->name : "";

          name_filter.each([&characterNames, &myName](const DisplayName &displayName, const CharacterTag &) {
            if (!displayName.name.empty() && displayName.name != myName) {
              characterNames += displayName.name + ", ";
            }
          });

          if (characterNames.length() >= 2) {
            characterNames.erase(characterNames.length() - 2);
          }

          if (characterNames.empty()) {
            response = "System: There are no other characters nearby.\n";
          } else {
            response = "System: The following characters are nearby: " + characterNames + "\n";
    }

    return ActionStatus::Done;
  }

  std::string getSuccessMessage() override {
    return response;
  }

  ActionStatus handleInterruption(flecs::entity) override {
    return ActionStatus::Interrupted;
  }

  void resume(flecs::entity) override {}

private:
  std::string response;
};
      // What this character remembers as a place. Not the map's list: places are
      // observed, so two characters can disagree, and an agent can be wrong
      // (docs/place-derivation-design.md section 10).
      class PlacesAction : public AgentAction {
      public:
        std::string getActionName() const override { return "PLACES"; }

        ActionStatus update(float, flecs::entity entity) override {
          const SpatialMemory *memory = entity.get<SpatialMemory>();
          const GamePosition here =
              entity.has<GamePosition>() ? *entity.get<GamePosition>()
                                         : GamePosition{0, 0};
          if (memory == nullptr || memory->places.empty()) {
            response = "System: You do not remember any places yet.\n";
            return ActionStatus::Done;
          }

          const std::vector<PlaceQuery::PlaceLine> sorted =
              PlaceQuery::SortedPlaces(*memory, here);

          response = "System: Places you remember (" +
                     std::to_string(sorted.size()) + "):\n";
          for (const PlaceQuery::PlaceLine &line : sorted) {
            response += "  " + PlaceQuery::FormatPlaceLine(line, here) + "\n";
          }
          response +=
              "System: [MOVE_TO] accepts these names. An unnamed place can be "
              "given one with [NAME_PLACE #<number> <name>], or with "
              "[NAME_PLACE here <name>] for the place you are standing in.\n";
          return ActionStatus::Done;
        }

        std::string getSuccessMessage() override { return response; }

        ActionStatus handleInterruption(flecs::entity) override {
          return ActionStatus::Interrupted;
        }

        void resume(flecs::entity) override {}

      private:
        std::string response;
      };

      // Names a place -- or makes one, when nothing covers the ground.
      //
      // The engine never invents a name, but it does record one the character
      // chose. Adding rather than replacing is deliberate: a place can be "the
      // Library" and "the old reading room" at once.
      class NamePlaceAction : public AgentAction {
      public:
        NamePlaceAction(std::string target, std::string label)
            : target(std::move(target)), label(std::move(label)) {}

        std::string getActionName() const override { return "NAME_PLACE"; }

        ActionStatus update(float, flecs::entity entity) override {
          SpatialMemory *memory = entity.get_mut<SpatialMemory>();
          if (memory == nullptr) {
            response = "System: You cannot name places right now.\n";
            return ActionStatus::Failed;
          }

          const std::string trimmed = StringUtils::Trim(label);
          if (trimmed.empty() || trimmed.size() > 48 ||
              trimmed.find('[') != std::string::npos ||
              trimmed.find(']') != std::string::npos) {
            response = "System: A place name must be 1-48 characters with no brackets.\n";
            return ActionStatus::Failed;
          }

          const bool here = StringUtils::EqualsIgnoreCase(target, "here");
          const GamePosition position = entity.has<GamePosition>()
                                            ? *entity.get<GamePosition>()
                                            : GamePosition{0, 0};

          KnownPlace *place = nullptr;
          if (here) {
            for (KnownPlace &candidate : memory->places) {
              const std::uint64_t key = VisionQuery::CellKey(position);
              if (std::find(candidate.memberCells.begin(),
                            candidate.memberCells.end(),
                            key) != candidate.memberCells.end()) {
                place = &candidate;
                break;
              }
            }
          } else {
            // The registry pattern accepts "#4"; the id is the digits.
            const std::string digits =
                target.empty() ? target : target.substr(target[0] == '#' ? 1 : 0);
            const int id = std::atoi(digits.c_str());
            for (KnownPlace &candidate : memory->places) {
              if (candidate.id == id) {
                place = &candidate;
                break;
              }
            }
            if (place == nullptr) {
              response =
                  "System: You do not remember a place #" + digits + ".\n";
              return ActionStatus::Failed;
            }
          }

          if (place != nullptr) {
            for (const std::string &existing : place->labels) {
              if (StringUtils::EqualsIgnoreCase(existing, trimmed)) {
                response = "System: That place is already called that.\n";
                return ActionStatus::Done;
              }
            }
            place->labels.push_back(trimmed);

            // A label added straight to the place would vanish if the place were
            // rebuilt under a new id, so the name is also recorded as a seed
            // pointing at its centre. Seeds survive derivation; labels ride on
            // identity. Adding is idempotent because of the label check above.
            const bool known = std::any_of(
                memory->nameSeeds.begin(), memory->nameSeeds.end(),
                [&trimmed](const PlaceNameSeed &seed) {
                  return StringUtils::EqualsIgnoreCase(seed.label, trimmed);
                });
            if (!known) {
              PlaceNameSeed seed;
              seed.label = trimmed;
              seed.anchor = {place->anchor.x + place->width / 2,
                             place->anchor.y + place->height / 2};
              seed.source = KnowledgeSource::Observed;
              seed.createsPlace = false;
              memory->nameSeeds.push_back(std::move(seed));
            }
            response = "System: You call this place " + trimmed + ".\n";
            return ActionStatus::Done;
          }

          if (!here) {
            response = "System: You cannot name a place you do not remember.\n";
            return ActionStatus::Failed;
          }

          // Nothing covers this ground, so the name makes a place of it. The next
          // derivation pass sees the seed and grows the extent from what has been
          // observed around it.
          PlaceNameSeed seed;
          seed.label = trimmed;
          seed.anchor = position;
          seed.source = KnowledgeSource::Observed;
          seed.createsPlace = true;
          seed.radius = DEFAULT_PLACE_NAME_RADIUS;
          memory->nameSeeds.push_back(std::move(seed));
          response = "System: You name this spot " + trimmed + ".\n";
          return ActionStatus::Done;
        }

        std::string getSuccessMessage() override { return response; }

        ActionStatus handleInterruption(flecs::entity) override {
          return ActionStatus::Interrupted;
        }

        void resume(flecs::entity) override {}

      private:
        std::string target;
        std::string label;
        std::string response;
      };

      // Whole-chain know-how for an activity: every recipe that trains it,
      // expanded down to raw materials, with each recipe's station and any
      // authored hint. Read-only and stateless -- see GuideBuilder and
      // docs/guide-command-design.md.
      //
      // Not the progression skill system: that is a numeric track the engine
      // advances by doing work. This only answers a question, and changes nothing.
      class GuideAction : public AgentAction {
      public:
        explicit GuideAction(std::string topic) : topic(std::move(topic)) {}

        std::string getActionName() const override { return "GUIDE"; }

        ActionStatus update(float, flecs::entity entity) override {
          // flecs::world is move-only, so bind the handle to a named local
          // before handing it to the builder.
          flecs::world world = entity.world();
          const std::vector<std::string> topics = GuideBuilder::Topics(world);
          if (topics.empty()) {
            response = "System: No guides are available in this world.\n";
            return ActionStatus::Done;
          }

          // An omitted argument is the discovery path. The valid set is listed on
          // demand rather than carried in every spawn prompt, which is the whole
          // point of a guide being a query.
          if (topic.empty()) {
            response = "System: You can ask for a guide on: " +
                       FormatNameList(topics) + ".\n";
            return ActionStatus::Done;
          }

          if (GuideBuilder::Resolve(topics, topic).empty()) {
            response = "System: '" + topic +
                       "' is not an activity you can ask about. Available: " +
                       FormatNameList(topics) + ".\n";
            return ActionStatus::Done;
          }

          response =
              "System: " + GuideBuilder::BuildTopicGuide(world, topic) + "\n";
          return ActionStatus::Done;
        }

        std::string getSuccessMessage() override { return response; }

        ActionStatus handleInterruption(flecs::entity) override {
          return ActionStatus::Interrupted;
        }

        void resume(flecs::entity) override {}

      private:
        std::string topic;
        std::string response;
      };

      // General reference on an item TYPE: the recipe and station that makes it,
      // or the objects that yield it when nothing crafts it.
      //
      // Deliberately does not recurse. A model that needs one step should not pay
      // for the whole tree, and it walks further by asking about an input -- the
      // answer names its ingredients, so the drill-down is self-teaching.
      //
      // Distinct from [EXAMINE_ITEM], which describes one specific item the agent
      // is carrying and what can be done with it. This is about the kind.
      class ItemInfoAction : public AgentAction {
      public:
        explicit ItemInfoAction(std::string item) : item(std::move(item)) {}

        std::string getActionName() const override { return "INFO"; }

        ActionStatus update(float, flecs::entity entity) override {
          // flecs::world is move-only, so bind the handle to a named local
          // before handing it to the builder.
          flecs::world world = entity.world();
          const std::vector<std::string> items = GuideBuilder::ItemIds(world);
          if (items.empty()) {
            response =
                "System: No item information is available in this world.\n";
            return ActionStatus::Done;
          }

          if (item.empty()) {
            response = "System: You can ask about these items: " +
                       FormatNameList(items) + ".\n";
            return ActionStatus::Done;
          }

          const std::string resolved = GuideBuilder::Resolve(items, item);
          if (resolved.empty()) {
            response = "System: '" + item +
                       "' is not an item you can ask about. Available: " +
                       FormatNameList(items) + ".\n";
            return ActionStatus::Done;
          }

          response =
              "System: " + GuideBuilder::BuildItemInfo(world, resolved) + "\n";
          return ActionStatus::Done;
        }

        std::string getSuccessMessage() override { return response; }

        ActionStatus handleInterruption(flecs::entity) override {
          return ActionStatus::Interrupted;
        }

        void resume(flecs::entity) override {}

      private:
        std::string item;
        std::string response;
      };

      // [HELP] explains the command vocabulary itself. Bare, it prints every
      // advertised command with its short description; given an argument it
      // resolves that one verb case-insensitively and prints the long form
      // registered next to it.
      //
      // Hidden commands stay queryable but are never listed: the trade verbs are
      // taught in context, so they are kept out of the discovery list while still
      // being explainable to an agent that already knows the word. [SET_TASKS] is
      // visible now that the wake digest no longer teaches it.
      //
      // Registered before the [GENERIC_INTERACT] catch-all, which matches
      // "[A-Z_]+" and would otherwise swallow the verb.
      class HelpAction : public AgentAction {
      public:
        explicit HelpAction(std::string topic) : topic(std::move(topic)) {}

        std::string getActionName() const override { return "HELP"; }

        // Which verb was asked about (empty for the bare form), so the debug
        // label reads "HELP: MOVE_TO".
        std::string getActionContext() const override { return topic; }

        ActionStatus update(float, flecs::entity) override {
          response = topic.empty() ? BuildIndex() : BuildDetail();
          return ActionStatus::Done;
        }

        std::string getSuccessMessage() override { return response; }

        ActionStatus handleInterruption(flecs::entity) override {
          return ActionStatus::Interrupted;
        }

        void resume(flecs::entity) override {}

      private:
        // Optional sections are skipped entirely when empty, so a verb with no
        // caveats does not print an empty "Notes:" heading.
        static void AppendSection(std::string &out, const std::string &title,
                                  const std::vector<std::string> &lines) {
          if (lines.empty()) {
            return;
          }
          out += title + ":\n";
          for (const auto &line : lines) {
            out += "  - " + line + "\n";
          }
        }

        std::string BuildIndex() const {
          const std::string list = GlobalCommandRegistry::BuildVisibleCommandList();
          if (list.empty()) {
            return "System: There are no commands available to explain.\n";
          }
          return "System: The following commands are available:\n" + list +
                 "\nUse [HELP <COMMAND>] for a full explanation of one, "
                 "e.g. [HELP MOVE_TO].\n";
        }

        std::string BuildDetail() const {
          const GlobalCommand *command = GlobalCommandRegistry::FindByVerb(topic);
          if (command == nullptr) {
            // Not a command: say so and restate how HELP works, rather than
            // guessing at a near match the model did not ask for.
            return "System: '" + topic +
                   "' is not a command I know. Use [HELP] to see every command, "
                   "or [HELP <COMMAND>] for details on one, e.g. [HELP MOVE_TO].\n";
          }

          std::string out = "System: " + command->format + "\n";
          if (!command->help.description.empty()) {
            out += command->help.description + "\n";
          }
          AppendSection(out, "Examples", command->help.examples);
          AppendSection(out, "Usage", command->help.usage);
          AppendSection(out, "Notes", command->help.notes);
          return out;
        }

        std::string topic;
        std::string response;
      };

      // Glyph grid of the tiles around the agent. Kept for the placement hint,
      // where [PLACE] consumes relative coordinates and a picture of the ground
      // is exactly what the model needs. It is not a general awareness command:
      // a grid renders one character per tile, so it cannot show an item lying on
      // the agent's own tile, drops anything without a DrawAscii, and gives no
      // coordinates to anchor offsets on.
      inline std::string GetSurroundingsRadar(flecs::entity entity, int radius) {
        const GamePosition currentPos = *entity.get<GamePosition>();
        Map *map = entity.world().get<MapResource>()->map;

        std::string grid = "";
        std::map<char, std::string> legend;

        // Mark the agent
        legend['@'] = "You";

        for (int y = currentPos.y - radius; y <= currentPos.y + radius; ++y) {
          for (int x = currentPos.x - radius; x <= currentPos.x + radius; ++x) {
            if (x == currentPos.x && y == currentPos.y) {
              grid += "@";
              continue;
            }

            char tileChar = '?';
            if (map && map->IsInBounds(x, y)) {
              auto tile = map->GetTile(x, y);
              if (tile) {
                tileChar = tile->ascii->ch;
                // Terrain is what placement validation checks against ("tilled
                // soil", "path"), so the grid has to name it. Without this the
                // agent can only see that two tiles look different, not which
                // one it is allowed to place on.
                legend[tileChar] = tile->name;
              }
            } else {
              legend['?'] = "outside the map";
            }

            entity.world().filter<GamePosition, DrawAscii, DisplayName>().each(
              [&](flecs::entity, const GamePosition& pos, const DrawAscii& ascii, const DisplayName& dName) {
                if (pos.x == x && pos.y == y) {
                  tileChar = ascii.ch;
                  legend[tileChar] = dName.name;
                }
              }
            );

            grid += tileChar;
          }
          grid += "\n";
        }

        std::string result = "System: Placement radar (" + std::to_string(radius*2+1) + "x" + std::to_string(radius*2+1) + " grid):\n```\n" + grid + "```\nLegend:\n";
        for (const auto& pair : legend) {
           result += std::string(1, pair.first) + " = " + pair.second + "\n";
        }
        return result;
      }

      class InventoryAction : public AgentAction {
      public:
        std::string getActionName() const override { return "INVENTORY"; }

        ActionStatus update(float, flecs::entity entity) override {
          // Stack identical items: one entry per distinct name, with "(N)"
          // appended when more than one is held. Shared with the chest examine
          // text so both readings of a container agree.
          //
          // A worn item is annotated rather than hidden: it is still carried, and
          // the annotation is what makes "the equipped scythe" distinguishable
          // from "a second scythe in the pack" (which stacks as its own entry).
          std::vector<std::string> itemNames;
          entity.each<Holds>([&](flecs::entity child) {
            if (child.is_alive() && child.has<DisplayName>()) {
              itemNames.push_back(child.get<DisplayName>()->name +
                                  EquippedSuffix(child));
            }
          });

          if (itemNames.empty()) {
            response = "System: Your inventory is empty.\n";
          } else {
            std::string inventoryList;
            for (const auto& entry : StringUtils::StackNames(itemNames)) {
              inventoryList += "- " + entry.first;
              if (entry.second > 1) {
                inventoryList += " (" + std::to_string(entry.second) + ")";
              }
              inventoryList += "\n";
            }
            response = "System: You are currently holding:\n" + inventoryList + 
                       "\nYou can use [EXAMINE_ITEM $ITEM_NAME] to see what actions are available for an item.";
          }

          return ActionStatus::Done;
        }

        std::string getSuccessMessage() override {
          return response;
        }
        
        ActionStatus handleInterruption(flecs::entity) override {
          return ActionStatus::Interrupted;
        }
        
        void resume(flecs::entity) override {}

      private:
        std::string response;
      };

      // The agent's own task list, on demand (docs/task-system-design.md section
      // 9). The wake prompt carries the digest only on the first wake, so this is
      // how the agent asks for its current list afterwards. It is also where the
      // task-kind vocabulary is taught now that [SET_TASKS] is advertised without
      // explaining its kind column.
      class TasksAction : public AgentAction {
      public:
        std::string getActionName() const override { return "TASKS"; }

        ActionStatus update(float, flecs::entity entity) override {
          // The kinds the engine can actually check. Built here rather than
          // baked into the digest text because the registry is live world state,
          // and because this is the one place the vocabulary is taught now.
          std::string kindHint;
          if (const TaskKindRegistryResource *kinds =
                  entity.world().get<TaskKindRegistryResource>()) {
            if (kinds->registry != nullptr && kinds->registry->Count() > 0) {
              kindHint =
                  "Task kinds the engine can check (put one in the kind column):\n";
              for (const TaskKind *kind : kinds->registry->GetAll()) {
                kindHint += "  " + kind->id + " -- " + kind->description + "\n";
              }
            }
          }

          const TaskList *list = entity.get<TaskList>();
          if (list == nullptr || list->goals.empty()) {
            // An empty list still teaches the vocabulary: an agent that has not
            // authored a task yet is exactly who needs to know what a kind is.
            response = "System: Your task list is empty.\n";
            if (!kindHint.empty()) {
              response += "\n" + kindHint;
            }
            return ActionStatus::Done;
          }

          response = RenderTaskDigest(*list, "", kindHint);
          return ActionStatus::Done;
        }

        std::string getSuccessMessage() override { return response; }

        ActionStatus handleInterruption(flecs::entity) override {
          return ActionStatus::Interrupted;
        }

        void resume(flecs::entity) override {}

      private:
        std::string response;
      };

      class ExamineItemAction : public AgentAction {
      public:
        ExamineItemAction(std::string itemName) : itemName(itemName) {}

        std::string getActionName() const override { return "EXAMINE_ITEM"; }

        std::string getActionContext() const override { return itemName; }

        ActionStatus update(float, flecs::entity entity) override {
          flecs::entity targetItem = flecs::entity::null();
          
          entity.each<Holds>([&](flecs::entity child) {
            if (child.is_alive() && child.has<DisplayName>()) {
              if (StringUtils::EqualsIgnoreCase(child.get<DisplayName>()->name, itemName)) {
                targetItem = child;
              }
            }
          });

          if (!targetItem.is_alive()) {
            response = "System: Invalid item name. You don't have an item named " + itemName + ".\n";
            return ActionStatus::Done;
          }

          std::string objName = targetItem.has<DisplayName>() ? targetItem.get<DisplayName>()->name : "Item";
          response = "System: Available actions for " + objName + ":\n";
          if (const Durability* durability = targetItem.get<Durability>()) {
            response += "System: " + objName + " has " +
                        std::to_string(durability->current) + " of " +
                        std::to_string(durability->max) +
                        " durability left; it breaks at 0.\n";
          }

          auto interactions = ItemInteractionRegistry::GetAvailableInteractions(targetItem);
          if (interactions.empty()) {
            response += "- None\n";
          } else {
            for (const auto& interaction : interactions) {
              response += FormatInteractionForAI(StringUtils::ToUpper(interaction.name),
                                                 interaction.description);
            }
          }

          return ActionStatus::Done;
        }

        std::string getSuccessMessage() override {
          return response;
        }
        
        ActionStatus handleInterruption(flecs::entity) override {
          return ActionStatus::Interrupted;
        }
        
        void resume(flecs::entity) override {}

      private:
        std::string itemName;
        std::string response;
      };

      // Puts a carried item on. The slot rules -- which slot kind, how many
      // instances, what to take off to make room -- live in Equipment.hpp and
      // the item definition, so this verb knows nothing about particular items.
      class EquipAction : public AgentAction {
      public:
        EquipAction(std::string itemName) : itemName(std::move(itemName)) {}

        std::string getActionName() const override { return "EQUIP"; }

        std::string getActionContext() const override { return itemName; }

        ActionStatus update(float, flecs::entity entity) override {
          flecs::entity item = FindHeldItem(entity, itemName);
          if (!item.is_alive()) {
            response = "System: You don't have an item named " + itemName + ".\n";
            return ActionStatus::Done;
          }
          response = EquipItem(entity, item, ItemsOf(entity.world())).message;
          return ActionStatus::Done;
        }

        std::string getSuccessMessage() override { return response; }

        ActionStatus handleInterruption(flecs::entity) override {
          return ActionStatus::Interrupted;
        }

        void resume(flecs::entity) override {}

      private:
        std::string itemName;
        std::string response;
      };

      // Takes one worn item off, leaving it in inventory.
      class UnequipAction : public AgentAction {
      public:
        UnequipAction(std::string itemName) : itemName(std::move(itemName)) {}

        std::string getActionName() const override { return "UNEQUIP"; }

        std::string getActionContext() const override { return itemName; }

        ActionStatus update(float, flecs::entity entity) override {
          flecs::entity item = FindHeldItem(entity, itemName);
          if (!item.is_alive()) {
            response = "System: You don't have an item named " + itemName + ".\n";
            return ActionStatus::Done;
          }
          response = UnequipItem(entity, item);
          return ActionStatus::Done;
        }

        std::string getSuccessMessage() override { return response; }

        ActionStatus handleInterruption(flecs::entity) override {
          return ActionStatus::Interrupted;
        }

        void resume(flecs::entity) override {}

      private:
        std::string itemName;
        std::string response;
      };

      // Lists what the character is wearing and where.
      class EquipmentAction : public AgentAction {
      public:
        std::string getActionName() const override { return "EQUIPMENT"; }

        ActionStatus update(float, flecs::entity entity) override {
          response = FormatEquipment(entity);
          return ActionStatus::Done;
        }

        std::string getSuccessMessage() override { return response; }

        ActionStatus handleInterruption(flecs::entity) override {
          return ActionStatus::Interrupted;
        }

        void resume(flecs::entity) override {}

      private:
        std::string response;
      };

      // Hidden verb: whole-list replacement of the agent's own task list
      // (docs/task-system-design.md section 9). It always reports back, because
      // a tolerant parse can still have skipped something and the agent is the
      // only one who can repair its own list. Failure is never fatal: a payload
      // that cannot be read at all leaves the previous list exactly as it was.
      //
      // Terminal goals are untouched by this verb -- it replaces everything
      // *below* the terminal line, which the parser keeps in place.
      class SetTasksAction : public AgentAction {
      public:
        explicit SetTasksAction(std::string payload)
            : payload(std::move(payload)) {}

        std::string getActionName() const override { return "SET_TASKS"; }

        ActionStatus update(float, flecs::entity entity) override {
          if (applied) {
            return ActionStatus::Done;
          }
          applied = true;

          TaskList current;
          if (const TaskList *existing = entity.get<TaskList>()) {
            current = *existing;
          }

          std::vector<std::string> warnings;
          const int tick = CurrentTaskTick(entity.world());
          const bool replaced =
              ParseSetTasksPayload(payload, current, tick, warnings);

          if (replaced) {
            const std::size_t goalCount = current.goals.size();
            entity.set<TaskList>(std::move(current));
            message = "System: Your task list has been updated (" +
                      std::to_string(goalCount) + " entries).\n";
          } else {
            message =
                "System: Your task list could not be read, so it is unchanged. "
                "Use: id | description | horizon | kind | target | parent | "
                "tier, one entry per line separated by ';'.\n";
          }
          for (const std::string &warning : warnings) {
            message += "  - " + warning + "\n";
          }
          return ActionStatus::Done;
        }

        std::string getSuccessMessage() override { return message; }

        ActionStatus handleInterruption(flecs::entity) override {
          return ActionStatus::Interrupted;
        }

        void resume(flecs::entity) override {}

      private:
        std::string payload;
        std::string message;
        bool applied = false;
      };

      class GenericInteractAction : public AgentAction {
      public:
        GenericInteractAction(std::string commandName, std::string commandArgs) : commandName(commandName), commandArgs(commandArgs) {}

        // Surfaces the actual verb the model emitted, e.g. "HARVEST" or "STORE".
        std::string getActionName() const override { return StringUtils::ToUpper(commandName); }

        // update() resolves the verb against an inventory item first and the
        // world InteractionTarget second; report whichever it landed on, plus
        // any extra argument (e.g. a crafting recipe) it was called with.
        std::string getActionContext() const override {
          if (resolvedTargetName.empty()) {
            return commandArgs;
          }
          if (commandArgs.empty()) {
            return resolvedTargetName;
          }
          return resolvedTargetName + " (" + commandArgs + ")";
        }

        ActionStatus update(float, flecs::entity entity) override {
          if (isFirstUpdate) {
            isFirstUpdate = false;
            bool found = false;

            // 1. Try to see if this is an Item Interaction (Inventory)
            if (!commandArgs.empty()) {
              flecs::entity targetItem = flecs::entity::null();
              entity.each<Holds>([&](flecs::entity child) {
                if (child.is_alive() && child.has<DisplayName>()) {
                  if (StringUtils::EqualsIgnoreCase(child.get<DisplayName>()->name, commandArgs)) {
                    targetItem = child;
                  }
                }
              });

              if (targetItem.is_alive()) {
                if (targetItem.has<DisplayName>()) {
                  resolvedTargetName = targetItem.get<DisplayName>()->name;
                }
                auto itemInteractions = ItemInteractionRegistry::GetAvailableInteractions(targetItem);
                for (const auto& interaction : itemInteractions) {
                  if (StringUtils::EqualsIgnoreCase(interaction.name, commandName)) {
                    successMsg = interaction.execute(entity, targetItem);
                    found = true;
                    break;
                  }
                }
              }
            }

            // 2. Fall back to World Interaction
            if (!found) {
              auto target = entity.get<InteractionTarget>();
              if (!target || !target->targetEntity.is_alive()) {
                successMsg = "System: Invalid command, or you are not near any interactive object. You must use [MOVE_TO $TARGET] first to interact with world objects. ";
                successMsg += HELP_HINT;
                successMsg += "\n";
                return ActionStatus::Done;
              }

              flecs::entity targetObj = target->targetEntity;
              if (targetObj.has<DisplayName>()) {
                resolvedTargetName = targetObj.get<DisplayName>()->name;
              }
              auto interactions = InteractionRegistry::GetAvailableInteractions(targetObj);

              for (const auto& interaction : interactions) {
                if (StringUtils::EqualsIgnoreCase(interaction.name, commandName)) {
                  successMsg = interaction.execute(entity, targetObj, commandArgs);
                  found = true;
                  break;
                }
              }

              if (!found) {
                successMsg = "System: Invalid command or action not available for this object. ";
                successMsg += HELP_HINT;
                successMsg += "\n";
              }
            }

            // The interaction may have started a timed action (e.g. [HARVEST] sets
            // Busy). Component writes are deferred inside an ECS system, so that
            // Busy is not visible yet: hold this frame and let the poll below
            // decide from the next tick. Returning Done here would drop the action
            // immediately and the agent would be prompted again mid-harvest.
            return ActionStatus::Doing;
          }

          if (entity.has<Busy>()) {
            return ActionStatus::Doing;
          }

          return ActionStatus::Done;
        }

        std::string getSuccessMessage() override {
          return successMsg;
        }
        
        ActionStatus handleInterruption(flecs::entity) override {
          return ActionStatus::Interrupted;
        }
        
        void resume(flecs::entity) override {}

      private:
        std::string commandName;
        std::string commandArgs;
        bool isFirstUpdate = true;
        std::string successMsg;
        // Name of the object/item update() matched the verb against. Empty until
        // the first update, so the debug label fills in a frame later.
        std::string resolvedTargetName;
      };

      // Sets a carried item down in the world at one or more relative
      // coordinates. What may be placed, where it may go and what it becomes are
      // declared by the item template's "placement" block, so this verb knows
      // nothing about particular items: "planting" is just placement where the
      // placed form happens to grow. See docs/placement-design.md.
      class PlaceAtAction : public AgentAction {
      public:
        PlaceAtAction(std::string itemName, std::string coordsStr)
            : itemName(itemName), coordsStr(coordsStr) {}

        std::string getActionName() const override { return "PLACE"; }

        // Which item is going down; the coordinates are already in the command
        // the model wrote.
        std::string getActionContext() const override { return itemName; }

        ActionStatus update(float, flecs::entity entity) override {
          if (isFirstUpdate) {
            isFirstUpdate = false;
            Place(entity);
          }
          return ActionStatus::Done;
        }

        std::string getSuccessMessage() override { return successMsg; }

        ActionStatus handleInterruption(flecs::entity) override {
          return ActionStatus::Interrupted;
        }

        void resume(flecs::entity) override {}

      private:
        // A coordinate that passed every rule, paired with the ground it will
        // land on so the reply can name it.
        struct Ready {
          std::pair<int, int> offset;
          std::string ground;
        };

        static std::string Offset(int dx, int dy) {
          return "(" + std::to_string(dx) + "," + std::to_string(dy) + ")";
        }

        static std::string JoinList(const std::vector<std::string>& values) {
          std::string joined;
          for (size_t i = 0; i < values.size(); ++i) {
            if (i > 0) joined += (i + 1 == values.size()) ? " or " : ", ";
            joined += values[i];
          }
          return joined;
        }

        void Place(flecs::entity entity) {
          std::vector<std::pair<int, int>> requested;
          int malformed = 0;
          std::istringstream iss(coordsStr);
          std::string coord;
          while (iss >> coord) {
            const size_t comma = coord.find(',');
            if (comma == std::string::npos) {
              ++malformed;
              continue;
            }
            try {
              requested.push_back({std::stoi(coord.substr(0, comma)),
                                   std::stoi(coord.substr(comma + 1))});
            } catch (...) {
              ++malformed;
            }
          }

          if (requested.empty()) {
            successMsg = "System: No usable coordinates. Use [PLACE " + itemName +
                         " AT X,Y ...], for example [PLACE " + itemName + " AT 1,0].\n";
            return;
          }

          Map *map = nullptr;
          if (auto *mapRes = entity.world().get<MapResource>()) {
            map = mapRes->map;
          }
          if (!map) {
            successMsg = "System: There is no map here to place anything on.\n";
            return;
          }

          // Collect first, then move: removing Holds mutates the relationship
          // this scan walks, and this action runs inside a system iteration,
          // which is a deferred context, so re-querying mid-loop would hand back
          // the same item on every pass. Same shape as the [STORE] handler.
          std::vector<flecs::entity> held;
          entity.each<Holds>([&](flecs::entity child) {
            if (child.is_alive() && child.has<DisplayName>() &&
                StringUtils::EqualsIgnoreCase(child.get<DisplayName>()->name, itemName)) {
              held.push_back(child);
            }
          });

          if (held.empty()) {
            successMsg = "System: You are not carrying any " + itemName + ".\n";
            return;
          }

          // Resolve the display name to a single item type. The policy lives on
          // the template and the project matches on ItemType rather than the
          // display name, so a same-named item of another type is never consumed
          // by this command.
          std::vector<flecs::entity> candidates;
          std::string targetType;
          for (flecs::entity item : held) {
            if (!item.has<Placeable>()) {
              continue;
            }
            const std::string type =
                item.has<ItemType>() ? item.get<ItemType>()->id : std::string();
            if (candidates.empty()) {
              targetType = type;
            } else if (type != targetType) {
              continue;
            }
            candidates.push_back(item);
          }

          if (candidates.empty()) {
            successMsg = "System: " + itemName + " cannot be placed in the world.\n";
            return;
          }

          const Placeable policy = *candidates.front().get<Placeable>();
          const GamePosition currentPos = *entity.get<GamePosition>();

          // Everything already standing on the map, gathered in one pass rather
          // than rescanning per coordinate. Placements queued earlier in this
          // same command are not visible here (deferred context), which is what
          // the duplicate check below covers.
          std::map<std::pair<int, int>, std::string> occupied;
          entity.world().filter<GamePosition, DisplayName>().each(
              [&](flecs::entity other, const GamePosition &pos, const DisplayName &name) {
                if (other != entity) {
                  occupied.emplace(std::make_pair(pos.x, pos.y), name.name);
                }
              });

          std::vector<Ready> ready;
          std::vector<std::string> rejected;
          std::set<std::pair<int, int>> seen;

          for (const auto &c : requested) {
            const std::string at = Offset(c.first, c.second);

            if (!seen.insert(c).second) {
              rejected.push_back(at + ": duplicate coordinate");
              continue;
            }

            const int distanceX = c.first < 0 ? -c.first : c.first;
            const int distanceY = c.second < 0 ? -c.second : c.second;
            const int distance = distanceX > distanceY ? distanceX : distanceY;
            if (distance == 0) {
              rejected.push_back(at + ": you are standing there");
              continue;
            }
            if (distance > policy.maxDistance) {
              rejected.push_back(at + ": " + std::to_string(distance) + " tiles away (max " +
                                 std::to_string(policy.maxDistance) + ")");
              continue;
            }

            const int x = currentPos.x + c.first;
            const int y = currentPos.y + c.second;
            if (!map->IsInBounds(x, y)) {
              rejected.push_back(at + ": outside the map");
              continue;
            }

            Tile *tile = map->GetTile(x, y);
            const std::string ground =
                (tile && !tile->name.empty()) ? tile->name : "unknown ground";
            if (tile && tile->blocksTile) {
              rejected.push_back(at + ": " + ground + " cannot be placed on");
              continue;
            }

            if (!policy.surface.empty()) {
              bool allowed = false;
              for (const auto &surface : policy.surface) {
                if (surface == ground) {
                  allowed = true;
                  break;
                }
              }
              if (!allowed) {
                rejected.push_back(at + ": " + ground + " is not " +
                                   JoinList(policy.surface));
                continue;
              }
            }

            const auto occupant = occupied.find(std::make_pair(x, y));
            if (occupant != occupied.end()) {
              rejected.push_back(at + ": occupied by " + occupant->second);
              continue;
            }

            ready.push_back({c, ground});
          }

          // Everything above is validation, so a rejected coordinate never costs
          // an item. Only now is anything consumed.
          ObjectFactory *factory = nullptr;
          if (auto *factoryRes = entity.world().get<ObjectFactoryResource>()) {
            factory = factoryRes->factory;
          }

          int placedCount = 0;
          std::string placedDetail;
          size_t next = 0;
          for (const Ready &target : ready) {
            if (next >= candidates.size()) {
              rejected.push_back(Offset(target.offset.first, target.offset.second) +
                                 ": no " + itemName + " left");
              continue;
            }
            flecs::entity item = candidates[next++];

            // Setting an item down takes it off first: the entity is reused as
            // the placed object, so a leftover Equipped marker would ride along.
            DropEquipped(item);
            entity.remove<Holds>(item);
            item.remove(flecs::ChildOf, entity);
            if (factory) {
              factory->ApplyTemplate(item, policy.becomes, map);
            }

            const GamePosition pos = {currentPos.x + target.offset.first,
                                      currentPos.y + target.offset.second};
            item.set<GamePosition>(pos);
            item.set<ScreenPosition>(map->GameCoordsToScreenCoords(pos.x, pos.y));

            placedDetail += "- " + Offset(target.offset.first, target.offset.second) + " " +
                            target.ground + "\n";
            ++placedCount;
          }

          if (malformed > 0) {
            rejected.push_back(std::to_string(malformed) +
                               " coordinate(s) could not be read");
          }

          if (placedCount > 0) {
            successMsg = "System: Placed " + std::to_string(placedCount) + " of " +
                         std::to_string(requested.size()) + " " + itemName + ":\n" +
                         placedDetail;
          } else {
            successMsg = "System: Nothing was placed.\n";
          }
          if (!rejected.empty()) {
            successMsg += "System: Not placed:\n";
            for (const auto &reason : rejected) {
              successMsg += "- " + reason + "\n";
            }
          }
        }

        std::string itemName;
        std::string coordsStr;
        bool isFirstUpdate = true;
        std::string successMsg;
      };

