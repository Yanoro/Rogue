# Agent attention and interruption

**Status:** design note, nothing implemented here.
**Scope:** how an agent becomes aware of the world while it is busy, how a System 1 model decides
whether that is worth waking it, and how a woken agent yields its current activity to talk.

**Companion document:** `docs/task-system-design.md` owns `TaskList`, the three goal horizons, tiers,
and the digest. This document consumes the digest as the *frame* that makes a relevance question
answerable, and produces the events that will later feed back into it. The seam is one rendered
struct; neither document requires the other to be implemented (§13).

**Prerequisite:** the task list, stages 1–4 of the companion document. The core insight of that
document is that a signal without a declared yardstick is noise — and the gate described here is
exactly the consumer of the yardstick.

---

## 1. Symptom

An agent executing an action cannot receive information. Concretely:

- `AgentBrain::executeActionQueue` appends to context only on `Done` and `Failed`
  (`src/AgentBrain.cpp:704-717`). While an action reports `Doing`, the agent learns nothing.
- `AgentBrain::update` re-arms `AgentSleepTimer{10}` on every frame of an executing action and
  early-returns while it is set (`src/AgentBrain.cpp:737-750`), so nothing re-deliberates mid-action.
- Nothing perceives. There is no system anywhere in `GameECS.cpp` that observes proximity, movement,
  or world changes on behalf of an NPC.

So two agents crossing paths exchange nothing. The moving one is not told what it passed, and the
standing one is not told it was passed. Nothing is broken — there is simply no channel.

### What already exists

This is the encouraging part, and it is why this design is mostly plumbing rather than new
machinery. The interruption *mechanism* is built and partly exercised:

| piece | location | state |
|---|---|---|
| `ActionStatus::Interrupted` | `include/AgentActions.h:34` | exists; 18 of the 19 action classes override `handleInterruption` in the header |
| `handleInterruption` / `resume` | `include/AgentActions.h:42-43` | `MoveToEntityAction` implements interruption by dropping `MOVE_THROUGH_PATH_ACTION` (`:234-237`) |
| the interrupt stack | `AgentBrain::interruptCurrentAction` / `resumeAction` | `src/AgentBrain.cpp:671-690` |
| a forced preemption | `AgentBrain::ForceInterruptAndPush` | `src/AgentBrain.cpp:557-560` |
| the one live caller | `TalkAction` constructor, preempting its *target* | `src/AgentBrain.cpp:64-69` |
| an "occupied" reservation | `Busy { flecs::entity actionEntity; }` | `include/Components.h:423`, used for workstations |

What is missing is a reason to pull the trigger. The existing caller is a player command reaching
another agent; nothing autonomous ever decides that an event is worth an interruption.

---

## 2. Symptoms this does and does not address

In scope:

- an agent learning that it passed someone, or that it was passed;
- an agent learning about non-social events (proximity, loud noise, growing hunger) at a defined
  granularity;
- a cheap gate that decides whether such an event is worth spending a full deliberation on;
- a bounded way for an agent to yield its activity and acknowledge someone.

Out of scope, and deliberately so:

- **Conversation mechanics.** Turn-taking, trade negotiation and `[EXIT]` already exist in
  `TalkAction`. This document only describes how a conversation *starts* mid-task.
- **Perception that requires eyes.** Cross-map knowledge, line of sight through walls, and hearing
  through geometry are not modelled. Radius plus an occlusion-free adjacency rule is the first
  version.
- **Player-facing UI for attention.** The debug surface is the goal.

---

## 3. Perception: engine facts, batched

A `Perception` component accumulates candidate events, and a system fills it from world state.

```cpp
enum class PerceptKind { AgentNearby, AgentDeparted, Noise, Hunger, TaskBlocked };

struct Percept {
  PerceptKind kind;
  flecs::entity subject = flecs::entity::null();  // who or what (may be null)
  std::string detail;      // "walking north", "hammering at the forge" -- engine-authored
  GamePosition where;
  int tick = 0;
  bool judged = false;     // has System 1 seen it yet?
  bool ignored = false;    // judged, verdict was "do not wake"
};

struct Perception {
  std::vector<Percept> events;      // bounded ring, DEFAULT_PERCEPTION_MAX
  float silenceUntilMs = 0.0f;      // suppression window after a wake or a talk (SS7)
};
```

### Detection

The first system is deliberately dumb: a transition detector, not a poller.

- For each agent, find other `CharacterTag` entities within `DEFAULT_PERCEPTION_RADIUS`.
- Emit `AgentNearby` on the **transition** into range, not every frame while in range — this is what
  keeps event volume proportional to *changes* rather than to time.
- Emit `AgentDeparted` on the transition out.
- Deduplicate against live events: one unjudged `AgentNearby` per subject.

Grid distance is the cheap version. `Map::AreNeighbours` already exists for the strict 8-neighbour
case (`src/Map.cpp:781`) and is reused as-is for "adjacent enough to greet"; the radius is for
*noticing*.

A full O(n²) scan over `filter<DisplayName, GamePosition, CharacterTag>()` is acceptable at village
scale and is what `MoveToEntityAction` already does for target resolution
(`include/AgentActions.h:159-166`). If it ever matters, the fix is a spatial index, not a redesign.

### Non-social percepts

`Noise` and `Hunger` are described here so the component shape does not need to change later, but
they are **not milestone-one work**. They differ in one important way: they need a source of truth
the engine does not currently have. Hunger in particular would have to become a real component with a
real rate before it can be perceived, which is a balance decision as much as a technical one. Listing
them here is to prevent the percept model from being hard-coded to `AgentNearby`.

---

## 4. The System 1 gate

### Decision in one line

Not *"does this event outrank the active task?"* but **"is this event worth waking the agent for?"**

That distinction is the whole reason this is tractable. The precise weighing — leave the ripe wheat
for one conversation, or acknowledge and keep walking — is the agent's call, made in full context
once awake. The gate only has to answer whether there is a plausible reason to interrupt, which means
its errors are cheap in one direction and merely missed opportunities in the other.

### Provider-agnostic interface

Mirrors the existing `AI` abstraction with its `OllamaAI`/`GeminiAI`/`OpenRouterAI` providers, so a
local ONNX model, a rules fallback, or a future provider can slot in without touching game code.

```cpp
// A typed classifier: state in, calibrated probabilities out. It does not generate text.
class WakeGate {
public:
  struct Question {
    std::string id;
    enum class Type { Noul, Score, Choice } type;
    std::string instructions;
    std::vector<std::string> options;  // Choice only
  };

  struct Verdict {
    float relevance = 0.0f;   // noul: P(this event concerns something the agent cares about)
    float urgency   = 0.0f;   // score: 0..1, how time-sensitive
    float familiarity = 0.0f; // noul: P(agent already knows this subject)
    bool  wake = false;       // derived
    std::string reason;       // echoed into the digest when woken
  };

  virtual ~WakeGate() = default;
  virtual void judge(const WakeState &state, const std::vector<Question> &questions,
                     std::function<void(Verdict)> onDone) = 0;
  virtual bool available() const = 0;
};
```

`available()` is what §12's degradation rule hangs on.

### What the gate sees

`WakeState`, assembled on the main thread from component state — all of it facts, none of it raw
history:

```
AGENT: Pietro (farmer)
TERMINAL GOAL: be an important member of the community
LONG-TERM GOAL: build standing through relationships
ACTIVE TASK: [HIGH] reap the wheat at the mill field   (engine facts: mill field ripe, hands empty)
CURRENT ACTIVITY: traveling north to the mill field, 8s elapsed
RECENT: greeted Margarethe 4 minutes ago

EVENT: Margarethe (blacksmith) is nearby at (12,7), walking north
```

Note what is deliberately absent: the chat transcript. A gate that reads full history is a full LLM
call with extra steps, which defeats the purpose entirely.

### Questions asked in one request

System 1 models evaluate every question in a request in parallel and charge almost nothing for the
additional ones, so the gate asks several at once rather than making several calls:

| question | type | serves |
|---|---|---|
| `relevant` | `noul` — "Does this event concern something the agent is trying to achieve?" | the gate, via the goal chain |
| `urgent` | `score` — "How time-sensitive is this?" | ranking when several events compete |
| `familiar` | `noul` — "Has the agent already engaged with this subject recently?" | suppression, so an ignored event does not re-fire |
| `which` | `choice` over nearby subjects | picking one when several are in range |

Only `relevant` is required. The others degrade to neutral if a backend does not support them.

**The terminal goal is the load-bearing input.** Without it the gate sees `[HIGH] reap the wheat` and
correctly declines to interrupt a passing NPC — suppressing exactly the social behaviour the terminal
goal wanted. This is the case the companion document's horizons exist for, and it is the reason the
two features are ordered the way they are.

### The wake rule is a threshold, and the threshold is not a tunable

```
wake = relevant >= DEFAULT_WAKE_RELEVANCE
       AND within rate limit
       AND not suppressed
       AND agent not already in a conversation
```

An earlier draft let the engine discreetly raise or lower urgency. That is rejected for the same
reason the companion document rejects semantic tier adjustment: an invisible third value between the
model's judgement and the agent's is the thing that makes priorities feel arbitrary. The gate
**reports a probability with a reason**, and the threshold decides. One knob, visible, and the
reason travels with the wake so the agent can disagree with it.

---

## 5. Flow, and the threading discipline

System 1 is a network call, so it cannot run on the main thread — and its verdict cannot mutate
agent state from wherever it lands.

```
PerceptionSystem        (main thread, ECS update)
   -> emits Percept, deduplicated, into Perception
   -> assembles WakeState, dispatches WakeGate on a worker thread
        |
        |  (worker)
        v
WakeGate (Jev)  -> Verdict
        |
        v
main-thread queue       (same pattern as g_AIResponseQueue, drained by
   -> writes WakeReason component, or marks Percept::ignored
   -> AgentBrain::update consults WakeReason at its next decision point
```

Three rules this must respect, all of which are already conventions in the tree:

1. **Verdicts do not call `ForceInterruptAndPush` directly.** They write a `WakeReason` component.
   `ForceInterruptAndPush` (`src/AgentBrain.cpp:557`) assumes it owns `currentAction`, and a system
   must not reach into the action stack. The gate advises; the brain decides where in its loop to
   act.
2. **Only the main thread writes context.** `AgentBrain::recordContext` is the documented single
   writer for `NPCContext::history` (`include/AgentBrain.h:85-88`), and the transcript is kept in
   sync through it. Nothing on the worker thread touches it.
3. **Deferred mutation inside ECS iteration.** `GameECS.cpp` already does this
   (`entity.world().defer(...)`, `src/AgentBrain.cpp:741`) and already documents the reentrancy
   hazard for agents contending over a shared resource (`src/GameECS.cpp:2494-2502`). Component
   writes from a perception system follow the same rule.

```cpp
struct WakeReason {
  std::string event;   // "Margarethe is nearby, walking north"
  float relevance = 0.0f;
  std::string reason;  // the gate's stated justification, shown to the agent
  int tick = 0;
};
```

### The ignore path is a feature

When the verdict is "do not wake," the event stays in `Perception` marked `ignored`. It is not
discarded. When the current action completes and the agent deliberates naturally, those events are
available to the digest with their age — so the agent still learns what it passed, just not at the
moment it happened.

That is the direct answer to *"he never gets the information that he passed by the other agent."*
Information is never lost; only its **timing** is gated. This property is why the gate can afford to
be conservative.

---

## 6. Yielding

Waking without yielding produces a frozen agent: `AgentBrain::update` cannot act on `WakeReason`
until the current action finishes, which may be arbitrarily far away. So the wake path and the yield
path have to land together.

**Reuse the existing interrupt stack, with a bounded action.** `MoveToEntityAction` already
implements `handleInterruption`, and `AgentBrain` already pushes the interrupted action onto
`actionStack` and resumes it (`src/AgentBrain.cpp:671-690`). The new piece is a `GreetAction` that
cannot run away with the conversation.

```cpp
class GreetAction : public AgentAction {
  // Acknowledge, allow at most DEFAULT_GREET_MAX_EXCHANGES turns of conversation,
  // then end on the agent's own initiative. Cannot extend itself.
  // On completion, the interrupted action resumes from the stack.
};
```

Why bounded, and why not `TalkAction`: `TalkAction` holds an entire conversation inside one
`AgentBrain` action slot, so a chatty partner blocks that agent's queue for as long as the
conversation runs. For a greeting that is unacceptable; for a deliberate player-initiated
conversation it is exactly right. The distinction is worth keeping: `TalkAction` is what an agent
does when it *means* to talk; `GreetAction` is what it does when it was interrupted and wants to
get back to work.

### Thrash is the primary risk

Two agents that both notice each other will both want to greet. Without suppression, a village
street becomes a standing wave of greetings and no work happens. Mitigations, in order of
importance:

1. **`Perception::silenceUntilMs`** — a suppression window after any wake or completed greeting.
2. **`familiar`** — the gate's own "have I engaged with this recently" answer, from
   `lastAdvancedTick`-style bookkeeping rather than from history.
3. **One greeting at a time.** A `Busy`-style social reservation. `Busy` already exists for
   workstation contention; whether to reuse it or add a sibling component is an open question (§13),
   but the mutual-notice race must be closed by *something* before behaviour is enabled.
4. **Per-agent rate limit** — at most one autonomous wake per `DEFAULT_WAKE_COOLDOWN_MS`.

---

## 7. Budget and observability

Autonomous waking makes deliberation cost proportional to *world activity* rather than to a timer,
which is the point — but it means the cost needs a ceiling and a readout.

- A global cap on concurrent wake-gate calls.
- A global cap on wakes per in-game minute. Exceeding it means the threshold is miscalibrated, not
  that the world is busy.
- Per-NPC wake counter, surfaced in the debug window: an NPC that wakes constantly is visible, not
  silent.
- Log every verdict — `(state, questions, verdict, reason, what the agent did next)`. This is the
  calibration set, and it is the only way to tune `DEFAULT_WAKE_RELEVANCE` with evidence rather than
  taste.

**This log is the argument for shipping in advisory mode.** Stages 1–4 (§12) produce the data
without changing behaviour. The threshold should be set from that data, not guessed.

---

## 8. Degradation

The feature is additive and inert. Nothing that exists today may break:

| condition | behaviour |
|---|---|
| No `TaskList` on the NPC | Gate still runs on raw facts, with no goal frame. Expect lower precision; do not crash. |
| `WakeGate::available()` false (no key, backend down) | No gate calls. Events accumulate and go unjudged, to be picked up at the next natural deliberation. |
| Gate times out or returns malformed output | Treat as "do not wake," log the event, continue. Never stall. |
| `Perception` grows past its cap | Drop oldest unjudged first, `ignored` before `judged`. |
| NPC has no `CharacterTag`/is not an agent | Never perceived as a subject. |

A missing verdict is always "ignore," never "interrupt" — the conservative direction, because a
missed greeting is invisible and a wake storm is not.

---

## 9. Tuning constants

All in `include/Defaults.h`, following its existing `DEFAULT_*` convention:

| constant | initial | purpose |
|---|---|---|
| `DEFAULT_PERCEPTION_RADIUS` | `8` | tiles within which an agent is *noticed* |
| `DEFAULT_AGENT_GREET_RADIUS` | `2` | tiles within which a greeting is possible |
| `DEFAULT_PERCEPTION_MAX` | `32` | events retained per agent |
| `DEFAULT_WAKE_RELEVANCE` | `0.6` | the gate's threshold; advisory mode sets this from data |
| `DEFAULT_WAKE_COOLDOWN_MS` | `30000` | per-agent suppression after a wake |
| `DEFAULT_GREET_MAX_EXCHANGES` | `2` | hard cap inside `GreetAction` |
| `DEFAULT_WAKE_MAX_CONCURRENT` | `4` | in-flight gate calls |
| `DEFAULT_WAKE_MAX_PER_MINUTE` | `10` | global wake-rate ceiling |

---

## 10. What this does not solve

Named so they are not mistaken for oversights:

- **Inference latency still exists.** A woken agent needs a full deliberation, and during it the
  agent stands still. The gate makes waking *rare and worth it*; it does not make the LLM fast. If
  this reads badly on screen, the fix is a non-LLM reflex layer that acknowledges instantly and
  delegates the decision — a later phase, not this one.
- **A conversation still blocks that agent's queue** for its duration. `GreetAction` bounds it;
  `TalkAction` does not.
- **Perception is radius-and-transition based.** No line of sight, no sound propagation, no memory
  of who is where.
- **The player is a separate code path** and is not a perceiving agent (an accepted divergence per
  the project direction).

---

## 11. Interaction with the other systems

| system | relationship |
|---|---|
| `TaskList` (companion doc) | supplies the goal frame and the active task; consumes ignored events at the next natural deliberation |
| `TalkAction` / trade | unchanged; `GreetAction` is a bounded sibling, not a replacement |
| `Busy` | candidate social reservation, shared with workstation contention — see §13 |
| `AIRequest` | a wake sets `AIRequest` through the normal path, with a task-aware prompt instead of `"What is your next command?"` |
| `AgentSleepTimer` | the current early-return is what makes mid-action awareness impossible; the wake path must be consulted before it returns |
| Debug windows | village board gains wake counts and last verdict; see companion doc §10 |

---

## 12. Implementation stages

Stages 1–4 **change no behaviour** and are individually useful.

1. **State.** `PerceptKind`, `Percept`, `Perception`, `WakeReason` components with reflection.
2. **Detection.** Perception system, transition-only emission, dedup, ring cap. Verify with the debug
   window alone: events appear, do not spam, and expire.
3. **Interface.** `WakeGate` abstraction; a `LogOnlyWakeGate` that records state and returns a fixed
   verdict. No provider, no network.
4. **Advisory mode.** Wire a real backend, log every `(state, questions, verdict, reason)`, and
   **count what the wake rate would have been.** Change nothing. Tune
   `DEFAULT_WAKE_RELEVANCE` from this data.
5. **Wake.** `WakeReason` consulted before `AgentSleepTimer`'s early return; prompt assembled with
   the digest and the event.
6. **Yield.** `GreetAction`, the interrupt-stack path, suppression and rate limits.

The transition from 4 to 5 is the decision point of the whole feature, and it is made with data
rather than up front. If the logged wake rate at a sane threshold is implausible, the answer is to
tighten the gate — not to weaken the perception layer.

---

## 13. Open questions

1. **Reuse `Busy` or add a social reservation?** `Busy` currently means "reserved by a workstation
   action" (`include/Components.h:423`), and its semantic is occupancy by an action entity. A
   conversation is occupancy by *another agent*. Overloading it risks confusing the two; a sibling
   component duplicates the contention logic. Undecided.
2. **Should proximity events for *known* vs *unknown* agents be distinguished by the engine?** Prior
   acquaintance is arguably a fact the engine can supply, rather than asking the gate to infer it
   from `familiar`. Leaning toward the engine supplying it.
3. **How is "recently" defined for suppression — wall clock, in-game ticks, or last-conversation
   tick?** The digest already tracks advancement ticks; the same clock could drive suppression.
4. **Does a greeting need the `TalkAction` machinery at all, or only an emote?** A wave that does not
   open a conversation channel is far cheaper and may capture most of the believability win. Worth
   testing both before committing to `GreetAction`'s full shape.
5. **Should the gate be consulted for events on `Terminal`-only agents** (an NPC with goals but no
   short tasks)? Untested; it may be the common case at spawn.
