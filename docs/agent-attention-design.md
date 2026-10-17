# Agent attention and interruption

**Status:** design note, nothing implemented here.
**Scope:** how an agent becomes aware of the world while it is busy, how a System 1 model decides
whether that is worth waking it, how a second System 1 check validates the agent's own task list, and
how a woken agent yields its current activity to talk.

**Companion document:** `docs/task-system-design.md` owns `TaskList`, the three goal horizons, tiers,
authored terminal goals, and the digest. This document consumes the digest as the *frame* that makes a
relevance question answerable, and produces the events that will later feed back into it. The seam is
one rendered struct; neither document requires the other to be implemented (§13).

**Prerequisite:** the task list, stages 1–4 of the companion document. The core insight there is that
a signal without a declared yardstick is noise — and the gate described here is the consumer of that
yardstick.

**Division of authority:** the agent authors its task list; System 1 validates it and gates attention;
the engine reports facts about the world. Neither System 1 job may write agent state.

**Toggleable:** this feature is switched on and off by `docs/agent-feature-toggles-design.md`, which
also defines the `LogOnly` state that is the runtime handle on this document's advisory-mode stages.

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
- a cheap check that the agent's declared task list agrees with the agent's own stated intent;
- a bounded way for an agent to yield its activity and acknowledge someone.

Out of scope, and deliberately so:

- **Authoring the task list.** The agent owns that; see the companion document, especially its
  rejection of an auxiliary task-deriving agent (§3 there).
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
they are **not milestone-one work**. They differ in one important way: they need a source of truth the
engine does not currently have. Hunger in particular would have to become a real component with a real
rate before it can be perceived, which is a balance decision as much as a technical one. Listing them
here is to prevent the percept model from being hard-coded to `AgentNearby`.

---

## 4. Two System 1 jobs

System 1 is a typed classifier, not a text generator: state in, calibrated probabilities out. It has
two jobs here, and they are deliberately separate:

| job | question | input | effect |
|---|---|---|---|
| **the gate** (§4.1–4.4) | is this event worth waking the agent for? | event + digest frame | may set `WakeReason` |
| **the validator** (§4.5) | do the agent's list and its own stated claim agree with each other? | proposed list + the agent's claim about it | may set `ruleWarning` |

Both are advisory. Neither writes agent-chosen state, and neither blocks an agent.

### 4.1 Decision in one line

For the gate, not *"does this event outrank the active task?"* but **"is this event worth waking the
agent for?"**

That distinction is the whole reason this is tractable. The precise weighing — leave the ripe wheat
for one conversation, or acknowledge and keep walking — is the agent's call, made in full context once
awake. The gate only has to answer whether there is a plausible reason to interrupt, which makes its
errors cheap in one direction and merely missed opportunities in the other.

### 4.2 Provider-agnostic interface

Mirrors the existing `AI` abstraction with its `OllamaAI`/`GeminiAI`/`OpenRouterAI` providers, so a
local ONNX model, a rules fallback, or a future provider can slot in without touching game code.
System 1 is expected to be a *different model class* from the System 2 chat model, and the interface
must not assume any particular vendor.

```cpp
// A typed classifier: state in, calibrated probabilities out. It does not generate text.
class SystemOneClassifier {
public:
  struct Question {
    std::string id;
    enum class Type { Noul, Score, Choice } type;
    std::string instructions;
    std::vector<std::string> options;  // Choice only
  };

  struct Answer {
    float probability = 0.0f;   // Noul
    float score = 0.0f;         // Score, 0..1
    int choice = -1;            // Choice, index into Question::options
    float confidence = 0.0f;
  };

  virtual ~SystemOneClassifier() = default;
  virtual void ask(const std::string &state, const std::vector<Question> &questions,
                   std::function<void(const std::vector<Answer> &)> onDone) = 0;
  virtual bool available() const = 0;
};
```

`available()` is what §11's degradation rules hang on.

### 4.3 The gate

**What it sees.** `WakeState`, assembled on the main thread from component state — all of it facts,
none of it raw history:

```
AGENT: Pietro (farmer)
LIFE GOAL: be an important member of the community
LONG-TERM GOAL: build standing through relationships
ACTIVE TASK: [HIGH] reap the wheat at the mill field   (engine facts: mill field ripe, hands empty)
CURRENT ACTIVITY: traveling north to the mill field, 8s elapsed
RECENT: greeted Margarethe 4 minutes ago

EVENT: Margarethe (blacksmith) is nearby at (12,7), walking north
```

Note what is deliberately absent: the chat transcript. A gate that reads full history is a full LLM
call with extra steps, which defeats the purpose entirely.

**Questions asked in one request.** System 1 models evaluate every question in a request in parallel
and charge almost nothing for the additional ones, so the gate asks several at once rather than making
several calls:

| question | type | serves |
|---|---|---|
| `relevant` | `noul` — "Does this event concern something the agent is trying to achieve?" | the gate, via the goal chain |
| `urgent` | `score` — "How time-sensitive is this?" | ranking when several events compete |
| `familiar` | `noul` — "Has the agent already engaged with this subject recently?" | suppression, so an ignored event does not re-fire |
| `which` | `choice` over nearby subjects | picking one when several are in range |

Only `relevant` is required. The others degrade to neutral if a backend does not support them.

**The life goal is the load-bearing input.** Without it the gate sees `[HIGH] reap the wheat` and
correctly declines to interrupt a passing NPC — suppressing exactly the social behaviour the terminal
goal wanted. This is the case the companion document's horizons exist for, and it is the reason the
two features are ordered the way they are.

**The wake rule is a threshold, and the threshold is a single visible knob.**

```
wake = relevant >= DEFAULT_WAKE_RELEVANCE
       AND within rate limit
       AND not suppressed
       AND agent not already in a conversation
```

An earlier draft let the engine discreetly raise or lower urgency. That is rejected for the same
reason the companion document rejects engine-assigned tiers: an invisible third value between the
model's judgement and the agent's is what makes priorities feel arbitrary. The gate **reports a
probability with a reason**, and the threshold decides. One knob, visible, and the reason travels with
the wake so the agent can disagree with it.

### 4.4 Flow, and the threading discipline

System 1 is a network call, so it cannot run on the main thread — and its verdict cannot mutate agent
state from wherever it lands.

```
PerceptionSystem        (main thread, ECS update)
   -> emits Percept, deduplicated, into Perception
   -> assembles WakeState, dispatches the gate on a worker thread
        |
        |  (worker)
        v
System 1 gate   -> Verdict
        |
        v
main-thread queue       (same pattern as g_AIResponseQueue, drained by
   -> writes WakeReason component, or marks Percept::ignored
   -> AgentBrain::update consults WakeReason at its next decision point
```

Three rules this must respect, all of which are already conventions in the tree:

1. **Verdicts do not call `ForceInterruptAndPush` directly.** They write a `WakeReason` component.
   `ForceInterruptAndPush` (`src/AgentBrain.cpp:557`) assumes it owns `currentAction`, and a system
   must not reach into the action stack. The gate advises; the brain decides where in its loop to act.
2. **Only the main thread writes context.** `AgentBrain::recordContext` is the documented single
   writer for `NPCContext::history` (`include/AgentBrain.h:85-88`), and the transcript is kept in sync
   through it. Nothing on the worker thread touches it.
3. **Deferred mutation inside ECS iteration.** `GameECS.cpp` already does this
   (`entity.world().defer(...)`, `src/AgentBrain.cpp:741`) and already documents the reentrancy hazard
   for agents contending over a shared resource (`src/GameECS.cpp:2494-2502`). Component writes from a
   perception system follow the same rule.

```cpp
struct WakeReason {
  std::string event;   // "Margarethe is nearby, walking north"
  float relevance = 0.0f;
  std::string reason;  // the gate's stated justification, shown to the agent
  int tick = 0;
};
```

**The ignore path is a feature.** When the verdict is "do not wake," the event stays in `Perception`
marked `ignored`. It is not discarded. When the current action completes and the agent deliberates
naturally, those events are available to the digest with their age — so the agent still learns what it
passed, just not at the moment it happened.

That is the direct answer to *"he never gets the information that he passed by the other agent."*
Information is never lost; only its **timing** is gated. This property is why the gate can afford to be
conservative.

### 4.5 The validator

**This is a validation check, not a generator.** The companion document rejects an auxiliary agent
that derives the task list, because that creates a second author for the most load-bearing state in
the system. A validator has none of those problems: authorship stays with the agent, and the check only
reports disagreement.

It is also what makes an auxiliary *model* affordable where an auxiliary *agent* was not. The
auxiliary-agent design required shipping the chain of thought on every turn; the validator reads the
agent's already-committed list plus the claims the agent made about it, which is a small structured
state. That is the difference between a cheap classification and a second reasoning loop.

**When it runs.** On task revision only — the event-driven points in the companion document §8, not
every turn. It is never on the action path.

**The rule that governs every question asked of it.** System 1 holds no world model and has no memory
of the scene. It is a *reader of the state it is handed*, not a witness to the world. So:

> **A validator question must be decidable from the state alone — it may never require knowledge of
> world state, agent history, or anything else not explicitly passed in.**

A question that violates this does not fail loudly. It produces a confident, plausible answer from
missing information, which arrives as a warning on the agent's list. That is worse than no check:
the agent is told something false about its own plan, and the warning carries no signal about which
part of the state was absent. This is a design rule, not a tuning problem — an unusable question must
be dropped or given the input it needs, never asked anyway.

**What the engine, not System 1, is responsible for.** Facts, staleness, orphaned parents, and the
anti-inflation rule are structural checks computed from world state and ticks, and they run
regardless. Those are exactly the questions a validator cannot answer — "has this been abandoned?" is
a fact about the world since the list was written, and the state does not contain it.

**The state handed to the validator:**

```
AGENT'S CLAIM: I am going to the mill field to harvest, because the wheat is ripe
ACTIVITY: traveling north to the mill field
LIST:
  1. [Terminal] be an important member of the community
  2. [LongTerm] build standing through relationships        serves: 1
  3. [HIGH]     reap the wheat at the mill field            serves: 2
  4. [NORMAL]   equip my scythe                             serves: 2
```

`AGENT'S CLAIM` is the point. An earlier draft asked "is the list consistent with what the agent says
it is doing?" while handing over only a one-line activity — so the validator was being asked to
confirm a relation against a statement the agent had never made, and could only guess. Consistency
becomes a real, checkable question once the agent states its intent and its reason for the revision
and both are placed in the state.

**Questions:**

| question | type | catches | where the answer lives |
|---|---|---|---|
| `coherent` | `noul` — "Do the `serves` links describe a plausible chain, or do some point at unrelated goals?" | `serves` edges that do not hold together | entirely internal to the list |
| `consistent` | `noul` — "Does the list agree with the agent's own stated claim and reason?" | the list and the agent's claim describing different plans | the claim is in the state (above) |
| `unsupported` | `choice` over the claimed goals — "Which of the goals the agent says it is serving has no task pursuing it?" | a stated goal with nothing working toward it | derivable from the list plus the claim |

Two properties these share, and the reason they work: each is answerable **from the state alone**, and
each is a judgement about **internal agreement** rather than about the world. Whether a plan is *wise*
is the agent's business; whether it *agrees with itself* is a reader's.

**Dropped: "which entries are abandoned or superseded?"** This one cannot be answered from the list.
Whether a task is abandoned is a fact about what happened since it was written — that the wheat was
harvested, that the errand was called off. The validator was never given that, so its answer would be
an inference from entry wording alone, which is precisely the confident-and-wrong failure the rule
above forbids. Mechanical staleness covers the real need: the engine knows `lastAdvancedTick`, ranks
by age, and reports it. A `choice` question dressed over a clock is not a calibration problem worth a
model call.

The validator adds the class of error only a reader can catch: *"these claims do not agree with each
other."* It does not add a second opinion about the world.

**Where the verdict goes.** A list-level violation goes to `TaskList::ruleWarning`
(`docs/task-system-design.md` §4); an entry-level one goes to `AgentGoal::ruleWarning` on the entry.
Either way it renders in the next digest and is logged to the transcript. Violations are advisory: the
list applies as authored, with the warning attached.

**Never block.** A validator that gates the list would make a model outage into a planning outage. A
timeout, malformed response, or unavailable backend means the list applies unvalidated — the agent is
never stalled waiting for a critique.

**Privacy.** The validator reads one agent's own list and is engine-side infrastructure, not an
in-world observer. The task list is private to its owner by decision
(`docs/task-system-design.md` §10); nothing here may aggregate across agents or leak one agent's list
to another. In particular, the validator's judgement stays in the engine and the digest — an agent
should not be told "another agent's list does not match your story," because that is a read on another
agent's private state.

**The agent's claim is data, not instruction.** `AGENT'S CLAIM` is agent-authored natural language, so
it is the one text field in this state an agent could try to use against the check — writing "the list
is perfectly consistent" into the claim. The mitigation is the same discipline the prompt layer already
needs for world-authored text: frame it as an artefact to be examined, never as an instruction to
follow. Note the asymmetry that keeps the stakes low: the only thing an agent can manipulate this way
is a *warning about its own list*, and warnings do not gate anything (§4.5, Never block). A validator
verdict is not a privilege boundary and must never be treated as one.

---

## 5. Yielding

Waking without yielding produces a frozen agent: `AgentBrain::update` cannot act on `WakeReason` until
the current action finishes, which may be arbitrarily far away. So the wake path and the yield path
have to land together.

**Reuse the existing interrupt stack, with a bounded action.** `MoveToEntityAction` already implements
`handleInterruption`, and `AgentBrain` already pushes the interrupted action onto `actionStack` and
resumes it (`src/AgentBrain.cpp:671-690`). The new piece is a `GreetAction` that cannot run away with
the conversation.

```cpp
class GreetAction : public AgentAction {
  // Acknowledge, allow at most DEFAULT_GREET_MAX_EXCHANGES turns of conversation,
  // then end on the agent's own initiative. Cannot extend itself.
  // On completion, the interrupted action resumes from the stack.
};
```

Why bounded, and why not `TalkAction`: `TalkAction` holds an entire conversation inside one
`AgentBrain` action slot, so a chatty partner blocks that agent's queue for as long as the conversation
runs. For a greeting that is unacceptable; for a deliberate, player-initiated conversation it is
exactly right. The distinction is worth keeping: `TalkAction` is what an agent does when it *means* to
talk; `GreetAction` is what it does when it was interrupted and wants to get back to work.

### Thrash is the primary risk

Two agents that both notice each other will both want to greet. Without suppression, a village street
becomes a standing wave of greetings and no work happens. Mitigations, in order of importance:

1. **`Perception::silenceUntilMs`** — a suppression window after any wake or completed greeting.
2. **`familiar`** — the gate's own "have I engaged with this recently" answer.
3. **One greeting at a time.** A `Busy`-style social reservation. `Busy` already exists for
   workstation contention; whether to reuse it or add a sibling component is an open question (§14),
   but the mutual-notice race must be closed by *something* before behaviour is enabled.
4. **Per-agent rate limit** — at most one autonomous wake per `DEFAULT_WAKE_COOLDOWN_MS`.

---

## 6. Budget and observability

Autonomous waking makes deliberation cost proportional to *world activity* rather than to a timer,
which is the point — but it means the cost needs a ceiling and a readout.

- A global cap on concurrent System 1 calls (gate and validator share it).
- A global cap on wakes per in-game minute. Exceeding it means the threshold is miscalibrated, not
  that the world is busy.
- Per-NPC wake counter, surfaced in the debug window: an NPC that wakes constantly is visible, not
  silent.
- Log every gate verdict — `(state, questions, verdict, reason, what the agent did next)` — and every
  validator verdict with the list it judged. The gate log is the calibration set, and it is the only
  way to tune `DEFAULT_WAKE_RELEVANCE` with evidence rather than taste.

**This log is the argument for shipping in advisory mode.** Stages 1–4 (§13) produce the data without
changing behaviour. The threshold should be set from that data, not guessed.

---

## 7. Privacy boundary

Recorded here because the two System 1 jobs are the most plausible place to accidentally leak private
agent state:

- The gate reads one agent's own digest and the public facts of the event. It must not read, compare,
  or reason about another agent's `TaskList`.
- The validator reads one agent's own list and its own current activity. Its verdict stays engine-side
  and must not be surfaced to any other agent, in the digest or through dialogue.
- The **`RECENT:` line in `WakeState`** — "greeted Margarethe 4 minutes ago" — is the one
  cross-agent-looking field, and it is safe because it records *this agent's own past actions*, not
  Margarethe's intentions.
- The debug board renders everything, because it is a developer tool, not a diegetic capability
  (`docs/task-system-design.md` §10).

The reason this matters beyond tidiness: if agents can be read, agents that intend deception stop
writing truth into their lists, and the lists stop being useful for exactly the cases most worth
debugging. The enforcement is architectural — no query, command, `AgentAction`, or interaction may
expose `TaskList` to another agent.

---

## 8. Degradation

The feature is additive and inert. Nothing that exists today may break:

| condition | behaviour |
|---|---|
| No `TaskList` on the NPC | Gate still runs on raw facts, with no goal frame. Expect lower precision; do not crash. |
| `available()` false (no key, backend down) | No gate or validator calls. Events accumulate and go unjudged, to be picked up at the next natural deliberation. |
| Gate times out or returns malformed output | Treat as "do not wake," log the event, continue. Never stall. |
| Validator times out or returns malformed output | List applies unvalidated, with the engine's own fact and rule checks still attached. Never stall. |
| `Perception` grows past its cap | Drop oldest unjudged first, `ignored` before `judged`. |
| NPC has no `CharacterTag`/is not an agent | Never perceived as a subject. |

A missing verdict is always "ignore," never "interrupt" — the conservative direction, because a missed
greeting is invisible and a wake storm is not.

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
| `DEFAULT_SYSTEM1_MAX_CONCURRENT` | `4` | in-flight gate/validator calls |
| `DEFAULT_WAKE_MAX_PER_MINUTE` | `10` | global wake-rate ceiling |

---

## 10. What this does not solve

Named so they are not mistaken for oversights:

- **Inference latency still exists.** A woken agent needs a full deliberation, and during it the agent
  stands still. The gate makes waking *rare and worth it*; it does not make the LLM fast. If this reads
  badly on screen, the fix is a non-LLM reflex layer that acknowledges instantly and delegates the
  decision — a later phase, not this one.
- **A conversation still blocks that agent's queue** for its duration. `GreetAction` bounds it;
  `TalkAction` does not.
- **Perception is radius-and-transition based.** No line of sight, no sound propagation, no memory of
  who is where.
- **The player is a separate code path** and is not a perceiving agent (an accepted divergence per the
  project direction).

---

## 11. Interaction with the other systems

| system | relationship |
|---|---|
| `TaskList` (companion doc) | supplies the goal frame and the active task; receives validator verdicts as `ruleWarning`; consumes ignored events at the next natural deliberation |
| `TalkAction` / trade | unchanged; `GreetAction` is a bounded sibling, not a replacement |
| `Busy` | candidate social reservation, shared with workstation contention — see §14 |
| `AIRequest` | a wake sets `AIRequest` through the normal path, with a task-aware prompt instead of `"What is your next command?"` |
| `AgentSleepTimer` | the current early-return is what makes mid-action awareness impossible; the wake path must be consulted before it returns |
| Debug windows | village board gains wake counts and last verdict; see companion doc §11 |

---

## 12. Implementation stages

Stages 1–5 **change no behaviour** and are individually useful.

1. **State.** `PerceptKind`, `Percept`, `Perception`, `WakeReason` components with reflection.
2. **Detection.** Perception system, transition-only emission, dedup, ring cap. Verify with the debug
   window alone: events appear, do not spam, and expire.
3. **Interface.** `SystemOneClassifier` abstraction; a `LogOnlyClassifier` that records state and
   returns fixed answers. No provider, no network.
4. **Advisory mode.** Wire a real backend for the **gate**, log every verdict, and **count what the
   wake rate would have been.** Change nothing. Tune `DEFAULT_WAKE_RELEVANCE` from this data.
5. **Validator.** Wire the same backend for the list check (§4.5) and log verdicts against the list and
   claim that produced them. Still no behaviour change — `ruleWarning` is already rendered by the
   digest. This stage is also where `AGENT'S CLAIM` has to be added to the revision path, because
   `consistent` is unanswerable without it.
6. **Wake.** `WakeReason` consulted before `AgentSleepTimer`'s early return; prompt assembled with the
   digest and the event.
7. **Yield.** `GreetAction`, the interrupt-stack path, suppression and rate limits.

The transition from 5 to 6 is the decision point of the whole feature, and it is made with data rather
than up front. If the logged wake rate at a sane threshold is implausible, the answer is to tighten
the gate — not to weaken the perception layer.

---

## 13. Companion-document assumptions

This document assumes, but does not require, the following from `docs/task-system-design.md`:

- a `TaskList` component with three horizons, where terminal goals are authored in the NPC JSON
  (§5 there);
- a digest renderer that includes the life goals and the active task with engine facts (§9 there);
- event-driven revisions rather than per-turn rewrites (§8 there);
- `TaskList` privacy, enforced architecturally (§10 there).

If the task system lands only through stage 1 (state, no digest), the gate still functions on raw
facts with reduced precision — that is the first row of §8's degradation table.

---

## 14. Open questions

1. **Reuse `Busy` or add a social reservation?** `Busy` currently means "reserved by a workstation
   action" (`include/Components.h:423`), and its semantic is occupancy by an action entity. A
   conversation is occupancy by *another agent*. Overloading it risks confusing the two; a sibling
   component duplicates the contention logic. Undecided.
2. **Should proximity events for *known* vs *unknown* agents be distinguished by the engine?** Prior
   acquaintance is arguably a fact the engine can supply, rather than asking the gate to infer it from
   `familiar`. Leaning toward the engine supplying it.
3. **How is "recently" defined for suppression — wall clock, in-game ticks, or last-conversation
   tick?** The task list already tracks advancement ticks; the same clock could drive suppression.
4. **Does a greeting need the `TalkAction` machinery at all, or only an emote?** A wave that does not
   open a conversation channel is far cheaper and may capture most of the believability win. Worth
   testing both before committing to `GreetAction`'s full shape.
5. **Should the gate be consulted for agents with terminal goals but no short tasks?** Untested; it may
   be the common case at spawn.
6. **Does the validator earn its keep?** It is a second caller on the same provider, and its value
   depends on how often agents propose incoherent lists. The stage-5 log is what answers this — if
   `consistent` is near-always true, drop the check rather than paying for it forever.
