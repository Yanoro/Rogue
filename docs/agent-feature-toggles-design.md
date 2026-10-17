# Agent feature toggles

**Status:** design note, nothing implemented here.
**Scope:** how the task system and the attention/interruption system are switched on and off, so
they can be evaluated against the game with and without them.
**Companion documents:** `docs/task-system-design.md` (the task system) and
`docs/agent-attention-design.md` (attention, the System 1 gate, the validator, and yielding).

Both features are speculative. Neither has been run. The purpose of these toggles is to make that
evaluable rather than assumed: the same build, the same map, with a feature in and out, so the
question "is this worth it?" has an answer that is not a matter of taste.

---

## 1. The two features

| feature | what it owns | companion doc |
|---|---|---|
| **Tasks** | the `TaskList` component, the digest, `[SET_TASKS]`, engine facts and rule reports, the validator's list check | `docs/task-system-design.md` |
| **Attention** | perception events, the System 1 gate, `WakeReason`, `GreetAction` and the yield path | `docs/agent-attention-design.md` |

They are toggled separately because they fail separately. "The gate fires too often" and "the task
list is wrong" are different problems with different fixes, and a single switch cannot tell them
apart. Attention also *depends* on Tasks for its goal frame: with tasks off, the gate still runs on
raw facts but with reduced precision (attention doc §8). That ordering is worth knowing when reading
results — attention-off/tasks-on is a clean comparison, attention-on/tasks-off is a degraded one.

---

## 2. Three states per feature

Each feature is tri-state. The middle state is the point of the whole design.

```cpp
enum class AgentFeatureState {
  Off,      // the feature does nothing and is not computed
  LogOnly,  // computed, recorded, observable -- but changes no behaviour
  On,       // fully active
};
```

### 2.1 What each state does

| | **Tasks** | **Attention** |
|---|---|---|
| **Off** | No digest, no `[SET_TASKS]` handling, no facts, no validator. The component may still exist but is inert. | No perception system, no events, no gate calls, no `WakeReason`, no `GreetAction`. |
| **LogOnly** | Digest rendered to the debug window and transcript but **not injected** into `AIRequest.prompt`; facts and rule violations computed and recorded; validator called, its verdicts logged and **not** written to `ruleWarning`. | Perception runs; the gate is called and every verdict is logged with the state that produced it; `WakeReason` is **not** written and `GreetAction` never fires. |
| **On** | Everything, including prompt injection and warning rendering. | Everything, including waking and yielding. |

### 2.2 Log Only is free of consequences; it is not free of calls

Worth stating plainly, because it is the most likely source of a surprise bill:

- **Log Only still calls System 1.** It has to — a gate that is not asked cannot produce the verdict
  you are trying to calibrate. So Log Only costs the same model calls as On. The ceiling in §6 is
  what keeps that bounded.
- **Log Only costs no System 2 tokens in Tasks.** The digest is rendered for observation but not
  injected, so the agent's prompt is unchanged.
- **Log Only changes no behaviour at all.** Nothing wakes, nothing yields, no warning reaches the
  agent. Play is identical to Off; only the logs differ.

This is the same advisory mode the companion documents already stage (attention doc §12 stages 4–5,
task doc §13 stage 4). The toggle is the runtime handle on those stages.

### 2.3 Why these three and not a numeric level

A `float featureStrength` or a per-subsystem matrix was considered and rejected. Three named states
are checkable, diffable, and loggable — the same reasoning that gives task tiers five named buckets
instead of a number (task doc §6). A matrix of five subsystems × three states is 243 combinations
that nobody will test and that makes a bug report ambiguous about what was running.

---

## 3. Scope: global, with a per-NPC override

Resolution order, highest first:

1. **Per-NPC runtime override** — set from the debug UI this session, persisted (§5).
2. **Per-NPC authored override** — declared in the map JSON (§4).
3. **Global setting** — the session-wide state from the toggle menu (§6).

Per-NPC override is what makes the strongest comparison available: two agents in the *same* village,
in the *same* world state, one with the feature and one without. Cross-session comparison is weaker,
because agent state does not survive a restart (task doc §14.5) and the world is not reproducible.

**An override is per feature, per NPC, and independent.** "Attention on for Pietro, off for everyone
else" is a legal and useful configuration. So is "tasks off globally, on for Margarethe."

**Component:**

```cpp
struct AgentFeatureOverrides {
  // Unset states fall through to the global setting. std::optional rather than a
  // sentinel value, so "no override" and "explicitly Off" are distinguishable --
  // a sentinel would make "Off" impossible to express as an override.
  std::optional<AgentFeatureState> tasks;
  std::optional<AgentFeatureState> attention;
};
```

**Resolution helper**, in one place, because two call sites disagreeing about precedence is exactly
how a toggle becomes untrustworthy:

```cpp
AgentFeatureState ResolveFeatureState(flecs::entity agent, AgentFeature feature);
```

---

## 4. Authored overrides in the map JSON

Per-NPC authored overrides live with the other authored NPC fields (`background`, `inventory`,
`skills`, `equipped`) — the same pattern the terminal goals use (task doc §5):

```json
{
  "name": "Pietro",
  "position": [13, 9],
  "background": "A farmer who has worked this land his whole life.",
  "goals": { "terminal": ["be an important member of the community"] },
  "agentFeatures": {
    "tasks": "on",
    "attention": "off"
  }
}
```

**Implementation path**, mirroring terminal goals exactly:

- `NPCData` (`include/Map.h:15-52`) gains the parsed override pair.
- `Map::LoadMap` reads `npcJson.value("background", "")` at `src/Map.cpp:197`; the
  `agentFeatures` object reads the same way.
- `Game::createNPC` (`src/GameECS.cpp:203`) stamps `AgentFeatureOverrides` at spawn.
- `Map::SaveMap` writes the `npcs[]` array back at `src/Map.cpp:553-561`. The comment there already
  establishes that fields `NPCData` does not model get dropped, so this must be modelled to round-trip.

Names are the serialised form of the enum (`"off"`, `"log"`, `"on"`), case-insensitive, and an
unrecognised value is reported as a load warning and treated as **no override** rather than as Off.
A typo must not silently disable a feature.

---

## 5. Persistence: remembered, but not in a tracked file

**Required behaviour:** a fresh install has both features **On**. Once a user turns a feature off, the
next session starts with it off. Off is remembered; absence means the default.

### Where it must not go

`settings.json` is **tracked by git** — only `settings.local.json` is ignored (`.gitignore`). Writing
runtime toggle changes into `settings.json` would dirty a tracked file on every session, producing
commit noise for what is ephemeral test state. Startup *defaults* may be authored there; runtime
state may not.

### Where it goes

**`debug_windows_state.json`** — the file `DebugWindowState` already loads and saves, whose path is
`settings.debug.windowStateFile` (`./debug_windows_state.json`, `include/Settings.h:281`), loaded at
`src/Game.cpp:76` and saved at `src/Game.cpp:637`.

This is the right home, and the filename is the only thing that gives the wrong impression. The file
is already the debug session's state store rather than a window-visibility list: it holds
`stopAllAI`, `defaultFontPath`, `blueprintZoom` and `designerPaneWidth` alongside the booleans
(`include/DebugWindowState.h:118-131`). Agent feature state belongs with those. Reusing it means no
second store, no second path setting, and no second load/save call — see §5.1.

```json
{
  "aStar": false,
  "aiMenu": true,
  "stopAllAI": false,
  "agentFeatures": {
    "tasks": "on",
    "attention": "off"
  },
  "npcFeatureOverrides": {
    "Pietro":     { "attention": "log" },
    "Margarethe": { "tasks": "off" }
  }
}
```

- **Absent file, absent key, or absent feature → default On.** This is what makes "fresh install is
  On" and "remember my last choice" the same rule instead of two. It also means an existing
  `debug_windows_state.json` written before this feature existed loads cleanly with both features On.
- **Saved with the rest of the state** at `src/Game.cpp:637`.
- **Per-NPC overrides are keyed by NPC name**, which is the stable identifier available in the map
  JSON. A saved override for a name not present in the loaded map is retained and ignored — deleting
  it would make a temporary map change destructive to the saved state.

### 5.1 Why not its own file

A separate `agent_feature_state.json` was the first draft. Reusing `debug_windows_state.json` is
better on every axis except the name:

| | own file | reuse `debug_windows_state.json` |
|---|---|---|
| persistence code | new constructor, `LoadState`, `SaveState`, path setting, gitignore entry | two fields on an existing class |
| failure mode | the two stores drift, or one is saved and the other is not | none — one write path |
| naming | accurate | `debug_windows_state.json` is already a misnomer (`stopAllAI`, zoom levels) |

If the name ever matters, the honest fix is renaming the file and its setting in one change — not a
second file. Recorded as an open question (§11.5) rather than done here, because a rename touches
every checkout.

### 5.2 One genuine reliability caveat

The file is written **on shutdown only**. A crash therefore loses any toggle changes made that
session, which is noticeably worse for these toggles than for window visibility: they exist so a
feature can be flipped mid-session and evaluated, so a crash at minute forty can lose exactly the
configuration that made the session worth running.

Recommendation: **write on toggle change**, not only on exit. It is a small file and toggles change
rarely, so the cost is irrelevant, and it makes the persisted state match what the session actually
ran with. This is a change in `SaveState` call frequency, not a new mechanism.

### 5.3 Gitignore

`debug_windows_state.json` is **not tracked** and **not ignored** — `git check-ignore` does not match
it, and it is absent from `git ls-files`. It is conventionally untracked, which means `git add -A`
would start tracking it and commit machine-written state including the NPC override block.

This is pre-existing and not caused by this feature, but it is worth one line in `.gitignore`
alongside the existing `agent_tasks.txt` entry, since the file is now carrying more than window
booleans.

### Precedence with map-authored overrides

A saved runtime override beats a map-authored one (§3). The consequence to be aware of: editing
`agentFeatures` in the map JSON will appear to have no effect for an NPC the debug UI has previously
overridden. The debug window must therefore show, per NPC, whether its state is **authored**,
**runtime-overridden**, or **global** — otherwise this is a silent trap.

---

## 6. Runtime toggle

A **Feature Toggles** section in the existing AI menu window (`AIMenuWindow`, `DebugWindows.cpp:1365`),
which already switches AI backends at runtime. Applied immediately, no restart and no map reload, so a
feature can be flipped mid-session and the same agents watched changing behaviour.

Contents:

- One tri-state control per feature (`Off` / `Log Only` / `On`) for the global default.
- A count of System 1 calls made this session, so cost is never a surprise (§2.2).
- Per-NPC override controls, for the NPC currently selected in the NPC menu — including a "clear
  override" action, because an override with no way back to the global state becomes a mystery the
  moment the session is forgotten.
- Each control shows the **effective** state after precedence, not just the stored one.

**Applying a change must be safe at any time.** Turning a feature Off mid-action must not strand an
agent, cancel an in-flight action, or leave a half-written component:

- Turning Tasks Off leaves the component in place but inert — no digest, no injection. A `TaskList`
  the agent already wrote is not deleted, because re-enabling should resume from reality rather than
  from an empty list.
- Turning Attention Off drops pending `WakeReason` and unjudged percepts, and leaves
  `Perception` intact for inspection. `GreetAction` already running is **allowed to finish** — it is
  bounded (attention doc §5), and cancelling a conversation mid-sentence to save a toggle is worse
  than the toggle.
- A toggle change never touches `NPCContext::history`. Context stays the single-writer concern of
  `AgentBrain` (attention doc §4.4).

---

## 7. Debug surface

The existing debug surfaces must work with a feature **Off**, not just On. This is the difference
between a toggle and a blackout:

- **The task board and NPC task section render regardless of state.** An agent's list is visible even
  when the feature is Off, so you can see what the agent *had* been doing. With the feature Off, the
  board shows the list as last written and marks it stale.
- **The perception log renders regardless of state**, and in `LogOnly` it is the primary artifact.
- **Every row shows the effective feature state and its source** (authored / runtime / global), per
  §5. A board that cannot explain why two identical agents behave differently is not a debug tool.
- **A session summary on exit**: calls made, wakes that would have fired, wakes that fired, validator
  verdicts by question. This is the number that answers "is it worth it" — not a subjective read of
  one play session.

---

## 8. Interaction with the staged plans

The toggles do not replace the implementation stages; they are the runtime controls for them.

| stage (attention doc §12) | reachable via |
|---|---|
| 4. Advisory mode | `Attention = LogOnly` |
| 5. Validator | `Tasks = LogOnly` (validator runs, warnings logged) |
| 6. Wake | `Attention = On` |
| 7. Yield | `Attention = On` |

A feature can be implemented fully and still left in `LogOnly` in the shipped default configuration
while it is being evaluated. That is the intended workflow, and it is why `LogOnly` is a first-class
state rather than a debug flag.

---

## 9. Tuning and defaults

| setting | default | location |
|---|---|---|
| `agentFeatures.tasks` | `"on"` | `settings.json` (authored default) |
| `agentFeatures.attention` | `"on"` | `settings.json` (authored default) |
| runtime state | absent → default | `debug_windows_state.json` (via `settings.debug.windowStateFile`) |

Constants in `include/Defaults.h`, following its `DEFAULT_*` convention:

| constant | initial | purpose |
|---|---|---|
| `DEFAULT_AGENT_FEATURE_TASKS` | `On` | first-run default, per §5 |
| `DEFAULT_AGENT_FEATURE_ATTENTION` | `On` | first-run default |

---

## 10. Implementation stages

1. **Enum and resolution.** `AgentFeatureState`, `AgentFeature`, `ResolveFeatureState`, and the
   `AgentFeatureOverrides` component with reflection. Both features consulted nowhere yet.
2. **Persistence.** Fields on `DebugWindowState` plus `LoadState`/`SaveState` handling, with
   absent-means-default semantics and save-on-change (§5.2). No new file and no new path setting.
3. **Menu.** Feature Toggles section in `AIMenuWindow`; effective-state display and System 1 call
   count.
4. **Authored overrides.** `NPCData` field, map JSON read/write, load warnings for bad values.
5. **Gating.** Both features consult `ResolveFeatureState` at their entry points — and, importantly,
   the debug surfaces stop consulting it (§7).

Stage 5 is where the toggles become meaningful, so stages 1–4 should land with the features still
unconditionally on. That keeps the toggle work from changing behaviour while it is being built.

---

## 11. Open questions

1. **Does `LogOnly` for Tasks need the digest to reach the agent at all to be a fair test?** The
   current design says no — injections are what change behaviour, so `LogOnly` excludes them. But a
   digest that never reaches the agent also never influences the list the agent writes, so
   `LogOnly` measures the feature *without* its own feedback loop. That is probably the right
   baseline, but it is a real caveat when reading results and should be stated in the summary.
2. **Should per-NPC runtime overrides persist at all?** They are useful across sessions for a
   long-running comparison and confusing six sessions later. §5 recommends persisting them keyed by
   name; the alternative is runtime-only, which is cleaner but loses the comparison on restart.
3. **Should a `LogOnly` session be marked in the transcript header?** Cheap, and it makes
   `logs/` artifacts self-describing — otherwise a log from a `LogOnly` run is indistinguishable
   from an `On` run that happened not to fire.
4. **What is the system-1 call ceiling when `LogOnly` is on for a long session?** The attention doc's
   ceiling (attention doc §9) is sized for the `On` path. `LogOnly` may need its own, lower cap,
   since it fires on events without the wake-rate limiter acting as a brake.
5. **Should `debug_windows_state.json` be renamed?** Reusing it is clearly right (§5.1), but it now
   holds agent feature state and per-NPC overrides on top of `stopAllAI`, zoom levels and font paths.
   The accurate name would be `debug_state.json`, and the honest change is the file and its
   `settings.debug.windowStateFile` setting together, with a fallback read of the old name so an
   existing checkout does not silently reset every window to its default. Deferred because it touches
   every checkout for a cosmetic gain.
