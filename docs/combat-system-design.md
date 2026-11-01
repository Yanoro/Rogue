# Combat: four candidate systems, and a recommendation

Status: **design exploration, nothing built.** This document exists to pick a shape before any
combat code is written. It does not describe behaviour that exists today.

The owner's conditions, restated:

1. **Simple** — for the implementer, and for the agent that has to reason about it.
2. **Agent-legible** — an LLM must be able to make *interesting* decisions, not just pick the
   highest-damage option.
3. **Wide builds** — an RPG needs several viable characters, not one stat curve.
4. **Extensible** — a user should be able to add a move, a status, a weapon or a whole encounter
   without touching C++.

Everything below is judged against those four, plus the project's existing direction
(`airogue-direction`): new mechanics are components, behaviours are named, prompts are assembled
from data, and the engine owns facts while agents own intentions.

---

## 1. The constraints that actually decide this

**The agent reads text and cannot see the board.** Every combat system is, for an NPC, a sequence of
messages: "you are hit for 9", "Roderick is bleeding", "you have 31/40 health". Four candidate systems
are not equally readable in prose.

**An LLM cannot do expected value.** It can compare two stated numbers; it cannot reliably reason
about a 65% chance over four exchanges. A system whose decisions are *probabilistic* needs the engine
to state the odds explicitly and mark them per option, or the agent will choose by vibes. That is
possible, but it is a cost the deterministic option does not pay.

**A model round-trip is seconds.** Combat cannot require reflex decisions. Whatever the core loop is,
it must either (a) run without the agent for stretches, or (b) resolve one discrete decision per
exchange, with the sim willing to wait.

**Combat is a social act in a village sim.** A fight between a blacksmith and a farmer is not only a
health bar; it is an injury that removes labour, a murder that removes a role, and a witnessed act
that should eventually cost reputation. A system that only models damage and death leaves the most
interesting decisions to a future social layer.

**The effect layer already exists and should not be duplicated.** Combat is, mechanically, "resolve a
number by folding contributions from stats, skills, items and statuses" — exactly what
`ResolveDuration` (`include/EffectResolver.hpp`) already does for harvesting and crafting, and exactly
what `Fold` (`include/Modifiers.hpp`) already guarantees (order-independent, clamped, explainable).
The strongest argument for any candidate is how much of it is *data feeding the existing fold* rather
than a parallel rules engine.

---

## 2. What exists to build on

| piece | where | why it matters for combat |
|---|---|---|
| `Fold` / `ModifierList` | `include/Modifiers.hpp` | Flat/Pct/Override plus gates; returns a **contribution breakdown**, so an outcome can be explained to the agent digit by digit |
| `StatLink` (`point`, `activity`) | `include/StatLinks.hpp` | Declarative "stat affects outcome"; `point` is an outcome noun, `activity` is an open string. A new activity is data-only |
| `SourceKind::{Race, Class, Effect}` | `include/StatView.hpp` | Reserved; `GatherSources` already has the one door where statuses, classes and races get added |
| `DurationResolver` + `HookRegistry` | `include/EffectResolver.hpp`, `include/Hooks.hpp` | Timed actions already carry explained, modifiable durations; hooks contribute, never decide |
| `ActionTimer` / `ActionActor` / `ActionTarget` / `Busy` | `include/Components.h` | The action-entity pattern: start a timed action, resolve on `ActionCompleted` |
| `HarvestResolutionSystem` / `CraftResolutionSystem` | `src/GameECS.cpp:1842`, `:1889` | The exact template for a combat resolution system, including "wake the agent once, with the result" |
| `Holds` / `Equipped` / `ItemType` / `Requires` | `include/ItemTypes.h`, `EquipmentTypes.h`, `Components.h` | Weapons are items with tags and tier-scaled effects; "needs an edge weapon" is already content |
| `SkillBlock` / `ApplyTraining` | `include/SkillTypes.h`, `SkillTraining.hpp` | A strike can train a weapon skill on the same path harvesting trains Farming |
| `Rng::Derived` | `include/Rng.hpp` | Deterministic, per-source streams if a system does want dice |
| `SpawnFloatingDamage` | `include/FloatingText.hpp:115` | The damage call is already written and waiting |
| `CharacterStatusWindow` | `src/CharacterStatusWindow.cpp` | Where health/stamina would be displayed |

**One gap worth flagging:** `docs/stat-effects-design.md` is referenced ten times across the headers
but does not exist in the tree (and has no history). The conventions are recoverable from the code —
`Fold`'s comment is especially explicit — but combat will deepen this layer, so it is worth
restoring or re-deciding that document rather than letting a third system accrete on conventions
nobody can read.

---

## 3. Four candidate systems

Each is described by its **rule**, the **decisions** it gives an agent, how it produces **build
variety**, where it **extends**, and what it **costs**.

### A. Exchange — deterministic, time-priced combat

**Rule.** No to-hit roll. An attack is a timed action; when its timer completes, damage is computed
by a fold and applied. What makes a fight close is *who is committed to what, and for how long*.

- Every move has a duration (`ActionTimer`), a damage fold, a reach, and a cost (stamina, durability).
- A heavy swing is slow and *interruptible*: hit someone during their wind-up and they lose it.
- Guard is a short timed action: it reduces incoming damage but spends the time you could have
  attacked in.
- Fleeing is a chase, decided by the movement systems that already exist (`Velocity`, `MaxSpeed`).
- Damage is deterministic, but a defender's evasion/armour *compresses* it (glancing blow), so nimble
  or armoured characters take less per exchange rather than being missed.

**What the agent decides.** Engage or leave; which move; when to guard; whether to spend stamina on a
finisher; whether to finish or let them yield. All with exact numbers visible: "your cut will land in
1.4s for 9; his hammer lands in 2.0s for 14; you have 31/40."

**Builds.** Defined by *verbs and costs*, not numbers: fast low-damage duelist (short durations,
counters), slow heavy brute (interruptible, high damage), reach weapon (attack from 2 tiles, weak up
close), archer (long wind-up, punished in melee), brawler (cheap, low damage, high stamina). Armour
trades initiative for mitigation. All of this is move/weapon/skill data.

**Extends at.** A new move is a JSON file. A new status is an `Effect` source plus a component. A new
damage interaction is a `damage`-point link or a named hook.

**Cost / risk.** Deterministic combat can collapse into "always use the best damage-per-second move".
Interest must come from costs, positioning, interruption, and stakes — which means the *content* has
to be tuned, not hidden behind dice. This is the main risk of the system; it is a design risk, not an
implementation one.

**Sketch.**

```
data/moves/cut.json
{
  "name": "Cut",
  "damage": { "base": 4 },
  "duration": { "base": 1.4 },
  "reach": 1,
  "interruptible": true,
  "requires": { "equippedTags": ["edge"], "skill": { "id": "blades", "level": 1 } },
  "train": { "blades": 6 }
}
```

### B. Contest — opposed roll with success bands

**Rule.** Each attack is one opposed check: attacker accuracy vs defender evasion, both folded from
the existing sources. The margin maps to a band — miss / graze / solid / critical — and the band
multiplies weapon damage. Statuses apply on a band.

**What the agent decides.** Target selection (high evasion vs high armour), risk/reward moves
(power attack: +damage, −accuracy), resource spending, when to retreat. Requires the engine to print
odds per option ("Solid hit: 68%") or the agent is choosing blind.

**Builds.** The classic wide spread: accuracy, evasion, crit, armour penetration, resistances. Very
natural for an RPG audience, and easy to make feel good early because variance masks balance.

**Extends at.** Same seam as A: bands are data, moves are data, `hit`/`damage` points feed the fold.

**Cost / risk.** The agent's plans fail by chance, and the agent cannot distinguish "bad plan" from
"bad roll"; it retries, which reads as a dumb loop. Balance is harder (bands plus coefficients).
Implementation is not much harder than A, but *making it legible to an LLM* is.

**Where it is genuinely better:** uncertainty. A system with no roll at all can feel like arithmetic
homework, and some players (and some agents) want the gamble.

### C. Stance — the duel triangle

**Rule.** A tiny universal verb set — strike, guard, feint, focus, shove, disengage — resolving on a
rock-paper-scissors triangle, with stats adjusting cost and severity rather than the outcome. A
*poise* pool breaks on a bad exchange, staggering the loser for a free follow-up.

**What the agent decides.** Reading the opponent and committing to a counter. Because opposing NPCs
are LLM agents too, a feint is a real bluff, which is unusually interesting in this game.

**Builds.** Weakest of the four: mostly stances and passives, and it struggles to express "conjurer
versus ranger". Build variety lives in the triangle rather than in character construction.

**Extends at.** Trivially — a new stance is one data entry. But it extends *sideways*, not deeply.

**Cost / risk.** It is a minigame bolted onto a simulation: it cares about fencing, not about the
harvest the farmer is missing. Repetitive over dozens of fights, and it rewards mind-games the agent
may not enjoy playing for long.

**Verdict.** Good as a *mechanic inside* a larger system (poise/stagger is worth stealing), poor as
the foundation.

### D. Wounds — no health bar; injuries as sources

**Rule.** No HP attrition. A hit creates a wound with a body part and a severity. A wound is a
`SourceKind::Effect` source feeding the existing fold: a wounded arm raises the duration of work and
lowers damage, a bad leg slows movement. A fight ends when a side yields, is incapacitated, or
flees. Healing is a skill, an item, and days of time.

**What the agent decides.** Whether the fight is worth a week of lost labour; where to strike; when
to stop. Injuries persist into the economy, which is exactly this project's identity.

**Builds.** Armour coverage, precision, medicine, pain tolerance, and races with different bodies
(a race without legs simply cannot be hamstrung).

**Extends at.** A wound is a data-defined effect with links; the resolver needs no new code. Very
elegant *once the effect layer is complete*.

**Cost / risk.** Scope, bookkeeping, and death spirals. It needs a recovery/time layer the game does
not have yet, and the agent must track several long-lived conditions.

**Verdict.** The right *long-term consequence layer*, wrong first system.

---

## 4. Comparison

| | A. Exchange | B. Contest | C. Stance | D. Wounds |
|---|---|---|---|---|
| Rules simple to explain | **high** | medium | **high** | low |
| Agent can reason exactly | **high** | low without printed odds | **high** | medium |
| Decision depth per exchange | **high** (time, cost, interrupt, reach) | medium | **high** | medium |
| Build breadth | **high** | **high** | low | medium-high |
| Implementation cost | medium | medium | low | high |
| Reuses the effect layer | **high** | **high** | low | **high** |
| Fits a real-time village sim | **high** | medium (wants turns) | low | **high** (but later) |
| Balance risk | medium | **high** | low | **high** |
| Extends without C++ | **high** | **high** | **high** | medium |

---

## 5. Recommendation

**Build A (Exchange) as the core loop. Add D's wounds later as the consequence layer. Use B only as
an optional, explicitly-priced risk layer for specific moves. Do not build C.**

Why A wins on the owner's four conditions:

- **Simple.** One rule: *every combat action takes time; damage is a fold; the fold is shown*. There
  is no encounter manager, no turn state machine, no initiative table. Fighters are just actors with
  timed actions — the same pattern as harvesting and crafting, so the ECS needs no new concept.
- **Agent-legible.** Exact numbers, no hidden variance. The agent's decision is a comparison it can
  actually do, and the `FoldResult` breakdown means the message can say *why*: "Cut for 9 (Iron
  Scythe +2, Strength 14 +1, Roderick's leather −3)."
- **Wide builds.** Builds are move sets × costs × reach × equipment, which is where variety lives in
  every good RPG, and all of it is content. A `blades` skill and a `brawling` skill can differ by
  *which moves they unlock*, not by a coefficient.
- **Extensible.** A move, a status, a weapon tag, or a whole fighter archetype is JSON plus existing
  components. New mechanics follow the house rule: a small component, a `RegisterComponent` entry,
  and — if needed — a named hook.

Three details that A needs in order to work in *this* codebase:

1. **Randomness, if any, is a gate, never a fold input.** `Modifiers.hpp` already states the rule:
   randomness decides *whether* a contribution is in the list, not what `Fold` does with it. A
   bleeding proc therefore rolls before the fold and is displayed as "Bleeding applied", so the
   arithmetic stays replayable and explainable.
2. **Combat is a conversation with the agent, not a UI.** Every exchange ends with a message to
   `AgentBrain::appendContext` (the harvest/craft contract), containing health, the opponent's state,
   and the available options. The brief on combat start should be generated from the same data the
   prompt already uses (moves, tags, skills), not written per role — direction constraint 2.
3. **The agent's last order stands while it thinks.** An LLM round-trip is seconds; a fight cannot
   freeze waiting for it. Either one command is one exchange (simplest, and what
   `CraftResolutionSystem` already does for crafts), or a committed stance repeats automatically until
   replaced. Pick one explicitly — this is the single biggest play-feel decision in the system.

Why not B as the core: it is the classic answer, but it asks an LLM to do probability, and this
project's whole effect layer is built to *explain* outcomes rather than hide them. B is still a good
addition for the moves that want to be a gamble ("a wild overhead: 40% to shatter a guard"), where the
risk *is* the decision and the engine can print the odds.

Why not C: it swaps the simulation's verbs for a fencing minigame, and its build variety is thin.
Take poise/stagger from it — a good idea that fits inside A as a status.

Why D later: it is the correct destination for consequences, but it needs recovery, time and UI that
do not exist yet, and it is a large amount of state to hand an agent before it has a working fight.

**Addendum (§9).** The owner's WoW-shaped proposal — lock-on, auto-attack, GCD and cooldown abilities
— is a better-constrained form of A, not a rival to it. Auto-attack removes A's latency problem,
lock-on makes the target explicit state, and casts plus interrupts give the agent reactive decisions
instead of rotation arithmetic. §9 assesses it, lists the guards, and revises the first slice to
lock-on plus auto-attack only.

---

## 6. First slice (what to build first)

Deliberately small: one weapon, one attack, no statuses, no skills, no death.

**Components** (in `Components.h`, registered in `RegisterComponents` — not `ObjectFactory`, these are
character components):

- `Vitals { float health; }` — current health only. Maximum is folded on demand from the `health`
  point (Constitution), the same way `StatView` falls back to baselines: one fact, no cached maximum
  to fall out of sync.
- `InCombat { flecs::entity opponent; }` — optional in slice 0; a target relationship may be enough.
- `AttackAction { flecs::entity target; std::string moveId; }` on the action entity, beside
  `ActionActor`/`ActionTarget`/`ActionTimer`, exactly like `CraftAction`.

**Data:**

- `data/moves/<id>.json` — `name`, `damage.base`, `duration.base`, `reach`, `interruptible`,
  `requires`, `train`. Loaded by a `MoveRegistry` beside `RecipeRegistry`.
- The first move is unarmed, so no item is required to test it.

**Effect points:** `damage` and `health`, both consumed by pure resolvers in a new
`CombatResolver.hpp` (modeled on `EffectResolver.hpp`), so they are testable without flecs.

**Verb:** `[ATTACK $TARGET]` registered in `Game::ECSInit` before `LoadMap`, with the same rich
`help` block the other commands carry — the prompt builder already renders it.

**System:** `AttackResolutionSystem` on `ActionActor`/`ActionTarget`/`AttackAction` with
`ActionCompleted`, mirroring `CraftResolutionSystem` (`src/GameECS.cpp:1889`): resolve, apply damage,
`SpawnFloatingDamage`, append one message to both agents, destruct the action entity.

**Ending:** at 0 health the target gets `Incapacitated` and the attacker is told. **No death yet** —
death is a deliberate later verb, because in a village sim killing should be a choice with
consequences, not a side effect of a number reaching zero.

**Then, in order:** guard + flee; a weapon and the `Requires` tag gate; a weapon skill with
`trainedBy: ["attack"]`; statuses as `Effect` sources; morale/surrender; wounds and recovery; the
player path; group fights.

---

## 7. Open questions — decisions needed before code

1. **Pacing.** One command = one exchange (slow, deliberate, fits a thinking LLM), or a committed
   stance that auto-exchanges until replaced (keeps a fight moving while the model thinks)? Slice 0
   can do the first and add the second, but the choice shapes the verbs.
2. **Lethality.** Is `Incapacitated` the default end, with killing a separate deliberate command? It
   is the village-sim answer; it is not the roguelike answer.
3. **Player.** Direction says the player stays a separate path and NPCs get mechanics first. Does
   slice 0 include the player as a *target* (so a hostile NPC can hit them), or is it NPC-vs-NPC
   only?
4. **Who fights.** Hostile NPCs are LLM agents (bandits) — expensive and slow but consistent. Wild
   animals and similar would want a cheap non-LLM controller. Which is combat for?
5. **Health versus wounds.** Is `Vitals` a permanent part of the design, or an interim bar that
   wounds replace? If the latter, keep `health` out of the prompt's vocabulary so the migration is
   not a rewrite of everything the agent knows.
6. **New stats or skills?** The five stats are enough mechanically; Perception could carry
   initiative/ambush and Charisma intimidation/surrender. New *skills* (`blades`, `brawling`,
   `medicine`) are data-only and probably the right place for build variety.

---

## 8. Non-goals

- No separate combat/encounter screen or mode. Combat is timed actions in the same world.
- No turn order table. Order emerges from action durations.
- No damage numbers on the player-side HUD until the player path is touched.
- No PvP balance, no party composition, no loot tables for corpses in the first slice.

---

## 9. Addendum — the WoW-shaped proposal (lock-on, auto-attack, cooldowns)

Owner's proposal: real-time combat with a locked target, auto-attack, and cooldown-gated abilities.

**Assessment: it is not a fifth family. It is system A with four mechanisms, and those mechanisms
solve A's biggest practical problem — an LLM round-trip is slower than an exchange — better than the
"last order stands" note in §5.1.** It is the recommended concrete shape of A, subject to the guards
in §9.4.

### 9.1 Why it fits this game

| mechanism | what it buys |
|---|---|
| **Lock-on** | "who am I fighting" becomes real state: a prompt summary, a range/line-of-sight check, a default target for every ability. Cheap to build, and it makes every combat message unambiguous |
| **Auto-attack** | the fight continues at the speed of the agent's thinking instead of waiting for it. The swing is just another duration fold, so it costs almost nothing to add |
| **GCD** | a rate limit that maps onto the model's cadence. Roughly one model round-trip per action is the right trade, and it stops an agent from spamming a whole kit in one tick |
| **Casts + interrupts** | the best decision an LLM can make in combat: read a telegraphed cast in text and stop it. `ActionProgressBarSystem` (`src/GameECS.cpp:492`) already draws a per-action bar, so the *visual* cast bar exists for free |
| **Cooldowns + resource** | build variety without new engine concepts: an ability is data, and its cost/cooldown is the trade |
| **Timed buffs/debuffs** | these are already `SourceKind::Effect` sources in waiting (`include/StatView.hpp`), so a buff is a link, not new machinery |

The result is a combat loop whose *decisions are reactive and textual* — "Roderick begins casting
Mend, 2.4s", "your guard is ready, your Bash has 3s left" — which is exactly the shape of problem
LLMs are good at, rather than rotation arithmetic.

### 9.2 How it would be held in the code

| concept | representation | existing analogue |
|---|---|---|
| locked target | `LockedTarget { flecs::entity }` (or a flecs relation); `[TARGET <name>]`, auto-acquire on hostility | `InteractionTarget` (`Components.h`) |
| auto-attack | `AutoAttack` + an `ActionTimer` on the character or a persistent action entity; swing duration folded from weapon speed | `HarvestAction` |
| ability in flight | `CastAction { abilityId }` on the action entity, with `ActionActor`/`ActionTarget`/`ActionTimer` | `CraftAction` (`Components.h`, resolved by `CraftResolutionSystem`) |
| cooldowns | `Cooldowns { std::vector<CooldownEntry> }` — a vector, not a map, the same reason `StatBlock` and `SkillBlock` are vectors (flecs reflection) | `SkillBlock` |
| GCD | `GlobalCooldown { float remaining; }`; auto-attack is deliberately *not* on it | — |
| resource | `Resource { std::string id; float current; float max; }`; regen resolved by a fold | `Vitals` |
| buffs / debuffs | `TimedEffect { id, remaining, stacks }` list; each is an `Effect` source in `GatherSources` | reserved `SourceKind::Effect` |
| ability definitions | `data/abilities/<id>.json` → `AbilityRegistry` | `RecipeRegistry`, `SkillRegistry` |
| damage / cast time | new fold points `damage`, plus `duration` with activity `"cast"` / `"attack"` | `ResolveDuration` |

All of it is the action-entity pattern the ECS already uses, so there is no encounter manager, no turn
order table, and no new update loop.

### 9.3 Keep from WoW, drop from WoW

**Keep:** the GCD, the swing timer, lock-on, instant-versus-cast, interruptible casts with a lockout,
cooldown-gated abilities, timed buffs/debuffs, a class resource.

**Drop:** hit/expertise/dodge/parry/crit tables, threat and aggro, armour-type formulas, the item-level
treadmill, and 20-button kits. Those are the parts that assume a UI, a group of five, and a boss.

### 9.4 Where it goes wrong, and the guards

1. **Rotation collapse.** An LLM will reduce a cooldown kit to a priority list within a few fights, and
   then stop deciding. The guard is **situational gates**: resistances/immunities, interrupt windows,
   adds, range and position, diminishing returns on a repeated ability, and real resource pressure. If
   a fixed priority list wins every time, the agent has no decision left.
2. **Auto-attack becoming the fight** (or being vestigial). Decide the share explicitly — roughly
   20–30% of damage as the floor that keeps the fight moving, abilities as the decisions.
3. **Ability bloat.** NPCs should carry **3–5 active abilities plus auto-attack**; players may carry
   more, but both read one definition. A wide build for a player is a wide *data* set, not a wide
   prompt.
4. **Hidden state.** Cooldowns, resource, buffs and range must be in every combat message, or the agent
   issues commands it cannot legally perform and learns the wrong lesson.
5. **Message spam.** Wake the agent on *events* (a cast starts, a target drops below a threshold, a
   cooldown comes up, a debuff lands), not on every swing. `appendContext` is already the channel; the
   discipline is choosing what is worth a wake-up.
6. **Latency asymmetry.** A 1.5s GCD with a 3s model round-trip silently costs the agent every third
   action. Since the player path is separate anyway, make the GCD **data per kind** — player ~1.5s,
   NPC 2–3s — rather than pretending both sides share one clock.
7. **The solved-rotation problem is acceptable only if the interesting decision is upstream**: engage
   or not, whom, with what, and at what cost to the village. That is the village-sim reading, and it is
   a better fit here than trying to out-WoW WoW.

### 9.5 Questions this adds

- GCD length, and whether a model's pending decision should pause it.
- The maximum number of active abilities for an NPC.
- Auto-attack's share of damage.
- Whether non-LLM fighters (wolves, a guard's cheap controller) exist at all.
- Whether abilities are shared with the player path now, or defined once and wired later.

### 9.6 Revised first slice

0. **Lock-on + auto-attack only.** No cooldowns, no abilities. This alone is a playable, agent-legible
   fight and validates the pacing — the riskiest assumption in the whole design.
1. One instant ability and one cast ability, with interrupt and lockout.
2. Cooldowns, resource, and timed buffs/debuffs.
3. Skill-granted abilities, weapon speed, armour, then the consequence layer (§3.D).

---

## 10. Implemented player slice (2026-10-31)

This section records what was actually built, and where it deliberately differs from the proposal
above. Read it as the current truth; §§3–9 are the reasoning that led here.

### 10.1 The skill model, as implemented

The vocabulary is **`SkillDef.type`**, not a separate `AbilityDef`:

| type | button | effects |
|---|---|---|
| `passive` | no | always on |
| `active` | yes, one-shot | applied on use |
| `hybrid` | yes | always-on effects **and** a use |
| `toggle` | yes, on/off | applied only while switched on |

`SkillDef` gained a `SkillActivation` block (`useWeapon`, `baseDamage`, `cooldown`, `range`,
`gcd`, `gcdSeconds`, `autoCast`, `xpPerUse`, `requiresTarget`). `SkillRegistry` parses both and
reconciles them: an active skill without an activation, or a passive skill with one, is reported at
load instead of becoming a skill nobody can use.

`SourceGatherer` now reads the type: passive and hybrid always contribute, a toggle contributes only
while its id is in the new `ActiveToggles` component, and an active skill contributes nothing
passively. That is what stops a toggle from applying before it is switched on.

### 10.2 What is in the game now

- **Data**: `data/skills/attack.json` (weapon-driven, auto-cast, off the GCD) and
  `power_strike.json` (14 damage, 4s cooldown, on the GCD); `data/items/iron_sword.json`
  (damage 6, swing 1.8s, durability 100, `consumedBy: ["attack"]`); `data/objects/iron_sword.json`
  and `training_dummy.json` (500 health, never dies, never hits back); `maps/Map_Training.json`
  (grass yard, authored `playerStart`, `playerWeapon`, two dummies).
- **Stat links**: Strength → `damage`/attack (flat 1.5 per point above baseline), Dexterity →
  `duration`/attack (−3% per point), Constitution → `health` (flat 4 per point). Max health is
  folded on demand (base 60), never cached.
- **Components**: `Vitals`, `LockedTarget`, `SkillCooldown(s)`, `GlobalCooldown`, `AutoCastSkills`,
  `SkillBar`, `ActiveToggles`, `TrainingDummy`.
- **One resolver**: `include/CombatResolver.hpp` — `ResolveDamage`, `ResolveMaxHealth`,
  `FindEquippedWeapon`, all through the ordinary `Fold`, so an outcome carries its contribution
  breakdown.
- **One door**: `UseSkill` in `GameECS.cpp` checks cooldown, GCD, target, range, weapon timing,
  folds damage, applies floating text and durability, grants skill XP, and returns a log line. The
  hotbar, auto-cast, and the future agent command all go through it.
- **Systems**: `SkillCooldownSystem` ticks cooldowns and the GCD; `CombatAutoCastSystem` fires the
  auto-cast skills in bar order (basic attack exempt from the GCD).
- **Input**: `1`–`9` activate bar slots, `Tab` cycles living targets within 15 tiles, clicking a
  creature with `Vitals` locks it.
- **HUD**: one draggable ImGui panel with health, target frame and the skill bar; cooldown sweep,
  green auto-cast pip, right-click → *Always casts*; position persisted in `hud_state.json`
  (ignored by git). Default anchor is the bottom of the screen until it is moved.
- **Log**: the window and menu now say **Log**; entries carry a category (Combat / General), combat
  lines are always shown, and the existing level filters apply only to General.
- **Verification**: `./build/AIRogue ./maps/Map_Training.json --combat-check` runs one real exchange
  headlessly (needs a display) and prints the target, the hit, the cooldown refusal and the damage.

### 10.3 Deliberate limits

- **Instant only.** No cast bars, channels or interrupts yet; the GCD and per-skill cooldowns are the
  pacing.
- **No mitigation.** Armour, evasion and damage types are not in this slice; the target is a dummy.
- **Toggles are modelled but not exercised.** `ActiveToggles` gates source gathering, but there is no
  toggle content and no on/off activation verb yet.
- **No death.** Anything without `TrainingDummy` is clamped at zero and logged as fallen; the death,
  yield and wound layers are later slices.
- **Everyone knows every active skill** (the chosen test shortcut). Class/known-skill gating waits for
  the class design.
- **Internal names.** The window is "Log", but the buffer class is still `DebugLog` and the window
  class `DebugLogWindow`; a file/class rename is a mechanical follow-up, kept out of this change to
  avoid recompiling the tree.
- **Numbers are a first pass**, chosen to be legible rather than balanced.

---

## 11. Portraits and the ability bar (implemented)

### 11.1 Portraits

- **Registry**: `PortraitRegistry` reads `data/portraits/*.json`. An entry is just an id and an
  image reference: `{ "sprite": "data/sprites/portrait_player.png" }`, or a sprite convention id
  (`portrait_player`) which resolves through the same art-pack → generated → `data/sprites` order as
  every other sprite.
- **Component**: `Portrait { std::string id; }` on the character. Assignment is authored, never
  derived from a name: a map NPC's `"portrait"`, `player.portrait` in settings, and a race's
  `"portrait"` as the fallback. No component means the HUD draws `data/sprites/unknown.png` (the
  orange question mark) rather than an empty box.
- **Objects too**: an object template can carry `"components": [..., "Portrait"]` with
  `"portrait": "training_dummy"`, which is how the dummy gets a face in the target panel.
- **Why explicit paths for the shipped art**: the hand-curated art pack is read-only, and with an art
  pack configured the convention lookup deliberately does not fall back to `data/sprites/`. The test
  portraits therefore point at `data/sprites/...` directly. A pack that wants to restyle them can
  supply the convention id instead and the data file only changes its `sprite` line.

### 11.2 HUD layout

Three independent panels, all draggable, all remembered in `hud_state.json`:

| panel | default | contents |
|---|---|---|
| player | top-left (24,24) | portrait, name, red HP bar with current/max |
| target | top-centre | the same for the current `LockedTarget`, hidden when there is none |
| ability bar | bottom-centre | one row of 12 slots |

`F10` resets every panel to its default position. A saved anchor that no longer fits the presentation
rectangle is discarded on the first frame, so a layout from another monitor cannot strand a panel.

### 11.3 Ability bar

- **One row, 12 slots**, matching the reference's width but not its three rows.
- Keys `1`–`9` and `0` cover the first ten; slots 11 and 12 are click-only until letter binds exist.
- Empty slots still draw as frames, like the reference.
- Each slot shows an optional `icon` from `SkillDef` (resolved as a path or convention id), else the
  skill name clipped to fit, plus the keybind, a cooldown sweep with seconds, and a green auto-cast
  pip. Right-click still opens **Always casts**.
- **Three procedural skins**, picked by the three swatches at the right of the top strip, cycled with `F9`, and persisted:
  1. **Warband** — iron and stone, gold keybinds, winged end caps, diamond boss above the centre.
  2. **Ironclad** — cold steel, a thin red accent line, rivets in each slot, keybinds below.
  3. **Runic** — chamfered slate slots, teal rune ticks along the top, a glow on ready skills.
- No PNG frame asset: the whole bar is `ImDrawList` primitives, so it has no art-pack dependency and
  scales with the window.

---

## 12. Casts, interrupts, armour, and a sparring dummy (implemented)

### 12.1 Casts

`SkillActivation` gained `cast`, `interruptible` and `interrupts`. A skill with a cast time starts an
action entity carrying an `ActionTimer` and a `CastAction`; the caster gets `Casting` and `Busy`, and
the cooldown starts **immediately** so an interrupted cast still costs the skill.

- `CastResolutionSystem` applies the effect when the timer completes. It re-checks range, so walking
  away makes the cast fizzle instead of landing across the room.
- The HUD portrait panels show a cast bar: blue for the player, orange for the target, with an
  "Interruptible" / "Uninterruptible" line. That bar is the telegraph.
- `Kick` is the first interrupt: instant, cheap damage, `interrupts: true`. Landing it cancels an
  interruptible cast and destroys its action, so the effect never resolves.
- `Heavy Swing` is the first cast-time skill: 18 damage, 1.2s wind-up, five-second cooldown.

### 12.2 Armour

Flat mitigation, floored at one point per hit:

```
damage = max(1, foldedDamage - ArmorOf(target))
```

`ArmorOf` sums an innate `Armor` component and the `armor` of every equipped item, so an armoured
dummy and a leather vest use the same rule. The target panel shows `Armour N`; damage lines append
`(armour N)` when it mattered. The training map starts the player in a leather vest (3), and its
second dummy wears mail (4).

### 12.3 The sparring dummy

`Aggressive` is the smallest non-LLM controller: a timer, a range, and one telegraphed strike. It
exists so incoming damage and interrupts are testable before the agent combat path. The sparring
dummy's strike is a 2s cast, so the player can step out of range or kick it apart. A player reduced to
zero is knocked out and restored rather than killed -- a placeholder until the death/recovery layer.

### 12.4 Verification

`--combat-check` now covers the whole loop: a basic attack, the cooldown refusal, a Heavy Swing cast
resolved through `ecs.progress()`, and a Kick that interrupts a standing cast. The aggressive dummy's
strike was verified separately: 8 damage against 3 armour landed for 5 and the player's health moved
from 60 to 55.

---

## 13. Combat call-outs (implemented)

Casting and use now say their ability name in the world, for every entity, with no "Casting:"/"Cast:"
prefix:

- **World cast label**: `ActionProgressBarSystem` draws the ability name on a small dark strip under
  the 4px action bar, for any action carrying a `CastAction`. That covers the player, the sparring
  dummy, and any future NPC caster, because it is driven by the action entity rather than by a HUD
  panel.
- **Instant uses**: a deliberate activation floats the ability name over the caster
  (`SpawnFloatingCallout`), on the same dark strip. Auto-cast passes `announce = false`, so the basic
  attack does not fill the screen with its name every swing.
- **Cast completion**: when a cast lands, its name floats over the caster too. A fizzle says nothing.
- **Interrupt**: `InterruptCast` floats a red `Interrupted!` over the caster whose cast was broken.
- **HUD**: the player/target portrait cast bars keep the compact `<name> 1.2s` reading.

`FloatingText::background` is the only addition to the floating-text component; damage and XP numbers
stay plain, so a bar behind every tick does not become visual noise. The call-out strips fade with the
text they explain.

---

## 14. Ranged delivery and the "Effect registry" question

### 14.1 One registry called "Effect" is the wrong axis

An arrow, a fireball, a heal and an AOE are not peers. They differ in three independent ways:

| axis | question | examples |
|---|---|---|
| **delivery** | how does it reach the target? | melee, projectile, beam, area, self |
| **impact** | what happens when it lands? | damage, heal, apply a condition |
| **presentation** | what does the player see? | arrow sprite, fireball sprite, cast label |

A single effect record would have to union all three, growing a set of optional fields that only
apply to some kinds (speed/size for a projectile, radius for an AOE, amount for a heal, status id for
a condition). That is the shape that becomes hard to validate and hard to extend. "Effect" also
already means something in this codebase: `SourceKind::Effect` is a timed source, and `StatLink`
effects are the declarative links. Reusing the word for a skill payload would make every conversation
about it ambiguous.

**Decision: one registry per axis, built when a second real case exists.**

- **`ProjectileRegistry`** (implemented) owns the flying object: sprite, speed, size.
- **`ImpactRegistry`** (later) owns what lands: damage, heal, apply-condition. It should appear when
  the second impact kind is real, not before -- a one-case abstraction is a guess with a schema.
- **`delivery`** (later) becomes a field on `SkillActivation` (`melee`, `projectile`, `area`, `self`)
  when the second delivery exists; today the optional `projectile` id is enough and says exactly one
  thing.

An arrow is `projectile delivery + damage impact (+ a poison condition)`; a fireball is the same with
a different sprite and impact; a heal is an impact with no delivery; an AOE is a delivery shape that
applies its impacts to several targets. None of those want to be one record.

### 14.2 Fireball, implemented

- `data/projectiles/fireball.json`: `{ "sprite": "data/sprites/projectile_fireball.png", "speed": 420, "size": 24 }`.
- `ProjectileRegistry` loads `data/projectiles/*.json`; a skill names one with
  `"projectile": "fireball"` in its activation.
- `SpawnSkillProjectile` copies the flight values and the impact facts (owner, target, skill id,
  label, base damage, interrupt flag) onto a `Projectile` entity, so a registry reload mid-flight
  cannot change what is already in the air.
- `ProjectileUpdateSystem` homes toward the target's visual centre and applies the impact through
  `ApplyHit` -- the same damage/armour/floating-text path as melee. A dead or unpositioned target
  makes it fizzle.
- `ProjectileRenderSystem` draws the sprite over the world, under the floating numbers.
- `Fireball`: 14 damage, range 8, 6s cooldown, instant launcher, bar slot 2.
- The cast path also supports projectiles: a cast-time skill that names one launches it at cast
  completion instead of applying damage directly. The current fireball is instant, so that branch is
  compiled but not the one the self-check exercises.
- Startup validation reports a skill naming a projectile the registry does not define; at use time
  the skill falls back to a direct impact so a content typo costs the flight, not the spell.

Known limits: the projectile ignores walls (no line-of-sight collision yet), homes rather than
following a ballistic arc, and the impact folds the shooter's current stats rather than a launch-time
snapshot. AOE and healing are deliberately not built yet.
