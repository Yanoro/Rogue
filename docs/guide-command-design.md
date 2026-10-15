# The `[GUIDE $TOPIC]` and `[INFO $ITEM]` commands

**Status:** implemented for agents.
**Date:** 2026-10-13.
**Scope:** agent-facing knowledge retrieval. Read-only. No player-facing path yet.

---

## 1. What this is

Two commands answer "how do I accomplish this, in *this* world?", deliberately split by token
cost:

- **`[GUIDE $TOPIC]`** — the whole crafting chain for an activity (e.g. `blacksmithing`),
  expanded transitively down to raw sources. The orientation view.
- **`[INFO $ITEM]`** — one hop for a single item *type* (e.g. `iron_scythe`): the recipe and
  station that makes it, or, when nothing crafts it, the objects that yield it. **No
  recursion.** The drill-down view; the model reaches the next hop by asking about an input.

In short: **`[INFO $ITEM]` is `[GUIDE $TOPIC]` at depth 0.** Both render through the same
one-recipe primitive, so they cannot disagree about how a recipe reads.

The answers are **derived from live content data** — recipes, object templates, stations and
the world's own entities — and blended with **hand-written hints** for the things the data
model cannot express. Generated skeleton, authored colour.

**It is a query, not a state change.** It does not grant capability, award XP, unlock verbs,
or create persistent state. There is no `LearnedGuides` component and nothing is saved: the
answer is recomputed on every call, so it is always current and can never drift out of sync
with the content. Learning *how* is deliberately separated from getting *better* at it.

### Not to be confused with

- **The progression skills.** `SkillRegistry`, `SkillDef`, `SkillBlock`, `SkillValue`,
  `data/skills/*.json` and `Trains` are a **numeric progression track** (0–45, five stages)
  whose magnitude scales outcomes via declared effects. That system is unrelated to this
  document and is not modified by it. See §2.
- **`[EXAMINE_ITEM $ITEM]`.** That command describes one **specific instance** the agent is
  carrying and what can be done with it. `[INFO]` describes an item **type** in the abstract —
  which is also why `[INFO]` was preferred over `[SOURCE]`, whose wording implied provenance
  rather than reference. The registered rule text states the distinction explicitly.

---

## 2. Why "guide" and not "skill"

"Skill" is already taken, in a way that would be permanently confusing if overloaded:

| existing | meaning |
|---|---|
| `SkillRegistry`, `SkillDef` | content database of progression tracks, `data/skills/*.json` |
| `SkillBlock`, `SkillValue` | a character's level in each track |
| `SkillXp`, `Trains` | how doing work moves a track |
| `SkillTypes.h`, `SkillXp.hpp` | the components and value types |
| `data/skills/` | the directory on disk |

On top of that, `SkillDef.hpp` already reserves a `[SKILLS]` command for the progression
display, in a comment describing `description` as *"Flavour for [SKILLS], never a mechanical
claim"*. Shipping an unrelated `[SKILLS]` that returns how-to instructions would make both
features unreadable.

**Naming used by this design:**

| thing | name |
|---|---|
| whole-chain command | `[GUIDE $TOPIC]` |
| one-hop command | `[INFO $ITEM]` |
| actions | `GuideAction`, `ItemInfoAction` (`include/AgentActions.h`) |
| derivation | `GuideBuilder` (`include/GuideBuilder.h`, `src/GuideBuilder.cpp`) |
| shared rendering | `FormatRecipe` (`include/RecipeFormatting.h`) |
| recipe hint field | `CraftRecipe::hint` |
| object hint component | `Hint` (`include/Components.h`) |
| the argument | a **topic** (`SkillRegistry` id) or an **item** (`ItemType::id`) |

---

## 3. The problem it solves

**Today, the crafting graph is discoverable only by physically walking it.**

Crafting is a component interaction on the `Workstation` component, not a global command.
The station→recipe link lives in object JSON, and the agent only sees what a station offers
when it is standing there:

- `data/objects/smeltery.json` declares `"recipes": ["iron_ingot"]`
- `data/objects/anvil.json` declares `"recipes": ["iron_scythe"]`
- `data/recipes/iron_ingot.json` consumes 1 `iron_ore` → 1 `iron_ingot`
- `data/recipes/iron_scythe.json` consumes 3 `iron_ingot` → 1 `iron_scythe`

The ancestry of an item is invisible until the agent has visited every intermediate station.
`MOVE_TO` arrival prints the station's own recipes (`FormatStationRecipes`, on the arrival path
in `src/GameECS.cpp`), which is one hop, at one place, right now.

**Consequence:** an agent cannot decide "I will make a scythe" and plan backwards to ore. It
can only stumble forward. That is the gap these commands close.

Note the shape of the fix. `[INFO]` returns the same one-hop answer a station already gives —
but without requiring the walk. `[GUIDE]` returns the ancestry the world never showed
anywhere, at any station.

**Why a model-initiated query is the right shape.** There is no goal component anywhere in
`include/` — intentions live in the model by design, per the standing decision that *the
engine owns facts and agents own intentions*. Recipes, stations and world contents are
engine facts. A guide command is simply the intention-holder asking the fact-holder a
question, which is why it needs no engine-side notion of a goal.

---

## 4. Locked decisions

| # | decision | consequence |
|---|---|---|
| 1 | Concept is named **Guide**; whole-chain command is `[GUIDE $TOPIC]` | no collision with progression skills |
| 2 | A separate verb **`[INFO $ITEM]`** covers the single-item case | separate namespace, and the token/depth contract is explicit rather than inferred |
| 3 | `[GUIDE]` shows the **whole chain**, regardless of skill level | gating on the progression skill is deferred, not rejected |
| 4 | `[INFO]` shows **exactly one hop, no recursion** | the model drills down by asking about an input, which keeps each call minimal |
| 5 | The chain is **derived** from registries and the world | cannot drift; no content authoring for the skeleton |
| 6 | **Hand-written hints** are blended in | covers what the data model cannot express |
| 7 | Hints live in **two places**: a recipe's `"hint"`, or an object template's `"hint"` | a raw material has no recipe, so it needs somewhere else to explain itself |
| 8 | Items are rendered as **content ids** (`iron_ingot`) | matches `FormatRecipe` and the exact token `[CRAFT ...]` needs |
| 9 | A raw material names its **source object type, not its location** | "from Iron Vein", never *where* the vein is |
| 10 | **No locations, no coordinates, no directions** anywhere | the model reasons about placement via `[LOCATIONS]` / `[SURROUNDINGS]` |
| 11 | **Read-only and stateless** | no XP, no unlock, no component, nothing saved |
| 12 | Output is **facts, not a route** | no "you should now do X"; the decision stays with the agent |
| 13 | **Agents only** for now | the player path is a separate code path by standing decision, and is not adapted here |
| 14 | The item **vocabulary is content-driven**; **presence is world-driven** | a sparse map cannot remove an item type from what `[INFO]` will answer, yet the answer still says whether this world yields it |
| 15 | **What work teaches is reported**, from the subject's own `trains` block | the agent can see that harvesting wheat advances farming, which nothing else in the game tells it |
| 16 | A subject's **`Requires` gate is reported** as a prerequisite | read through the same parser the spawner uses, so the guide can never promise a harvest the engine will refuse |

Decision 15 comes from a real gap: `Trains` and `CraftRecipe::trains` are **the only** record of
what a piece of work teaches, and the fact lives on the *subject* rather than the skill (see
`SkillXp.hpp`). Nothing surfaces it to an agent, so before this the model could grind a skill
for hours without ever learning that the work was what moved it.

Decisions 9, 10 and 12 exist for the same reason: a guide that names the vein's coordinates and
hands over a step-by-step route replaces exploration with execution, and agent improvisation
is the point of the simulation. Naming *that* iron comes from an Iron Vein is a fact; saying
where that vein is would be playing the game.

---

## 5. How the answer is derived

### 5.1 Sources

| what | where |
|---|---|
| the set of topics | `SkillRegistry::GetIds()` |
| recipes belonging to a topic | every `CraftRecipe` whose `trains` contains that `skillId` |
| recipe inputs / outputs / time | `CraftRecipe::inputs` (`ItemStack`), `::outputs` (`LootDrop`), `::craftTimeSeconds` |
| recipe hint | `CraftRecipe::hint`, from a recipe's `"hint"` key |
| which station makes a recipe | entities with `Workstation` whose `recipes` contain the recipe id → `DisplayName::name` |
| where a raw item comes from | entities with `Harvestable` whose `lootTable.drops` include the item id → `DisplayName::name` |
| the item vocabulary `[INFO]` accepts | recipe outputs, plus the `"drops"` of every template listing `Harvestable` — **content, not world instances** (decision 14) |
| what a recipe teaches | `CraftRecipe::trains`, from a recipe's `"trains"` key |
| what harvesting teaches | the `Trains` component on the object, stamped from a template's `"trains"` key |
| which harvestable subjects teach a topic | every template listing `Harvestable` with a `"trains"` grant for that skill — **content**, re-parsed with the shared `ParseTrainingGrants` |
| what a subject requires | the `Requires` component on the object, or a template's `"requires"` block parsed with the shared `ParseRequiredTags` |
| raw-source hint | the `Hint` component on that object, stamped from the template's `"hint"` key |
| content identity | `ItemType::id` — the `ObjectFactory` template key, the same id space as recipe ids |

Topics are the **existing skill ids**. That is deliberate: it gives the argument a namespace
that already exists in data, matches how content is authored (`iron_ingot.json` declares
`"trains": {"blacksmithing": 5}`), and means a genre pack's new activities need no new
registry. A recipe may legitimately belong to more than one topic; that is a union.

### 5.2 Algorithm

```
GuideForTopic(topic):                       # [GUIDE blacksmithing]
    roots    = recipes whose trains contains topic, in id order
    trainers = harvestable TEMPLATES whose trains contains topic, by display name
    if roots is non-empty:
        emit "<display name> - N recipe(s)."
        for each root: Expand(root, depth = 0, indent = 0)
    if trainers is non-empty:
        emit "Trained by harvesting:"
        for each: emit "- <DisplayName> (harvestable)" + Clauses()
                  emit "  hint: <template hint>"      # omitted when absent
    if both are empty:
        emit "<display name> - nothing in this world makes or trains this."
        and name the skill's trainedBy guard, if any

GuideForItem(item):                         # [INFO iron_scythe]
    producers = every recipe whose outputs contain item
    if producers is non-empty:
        for each: RecipeLine at indent 0        # one hop, no recursion
        return
    AppendRawSources(item, indent = 0, bullet = false)   # terminal: it is gathered

Expand(recipe, depth, indent):
    RecipeLine(recipe, indent, bullet = true)
    if depth >= kDepthCap:
        emit "(truncated at depth N; step further with [INFO <item>])"
        return
    for each input, in order:
        producers = every recipe whose outputs contain input
        if producers is non-empty: Expand(producers.first, depth + 1, indent + 1)
        else:                      AppendRawSources(input, indent + 1, bullet = true)

RecipeLine(recipe, indent):                 # shared by both verbs
    emit "<indent>- <FormatRecipe(recipe)> @ <station DisplayName>" + Clauses()

AppendRawSources(item, indent, bullet):
    for each DISTINCT Harvestable object TYPE in the world whose drops include item:
        emit "<indent><item>: raw, from <DisplayName> (harvestable>" + Clauses()
        emit "<indent>  hint: <object Hint>"   # omitted when absent
    if none: emit "<indent><item>: raw; no source found in this world"

Clauses():                                  # appended only when they say something
    if the subject is gated: emit " - Requires equipped: <tags, comma separated>"
    if it teaches a skill:   emit " - Gives <n> <skillId> XP[, <n> <skillId> XP...]"
```

`RecipeLine` is the whole of `[INFO]`'s crafting behaviour, and `Expand` is a tree walk over
the same primitive.

Rules worth keeping:

- **Report, do not skip, the unknown.** `FormatStationRecipes` prints
  `(unknown: no data/recipes/<id>.json)` rather than omitting a broken reference, because a
  silent gap is indistinguishable from "cannot be done" and hides a content bug. The guide
  says `no source found in this world` for the same reason.
- **A topic with nothing behind it is reported, not hidden.** An activity can be declared with
  neither a recipe nor a harvestable subject; the guide then says so and names the skill's
  `trainedBy` guard, which is true and more useful than an empty list.
- **One line per source TYPE, not per instance.** A map with three iron veins is making one
  point about where ore comes from, and the answer is replayed in full on every later request
  (§9), so repeating it verbatim is a permanent cost for no information. Deduplicated on
  `ItemType::id`, so two *different* objects that both yield the item are still both reported.
  This was a real bug the first cut shipped, caught by replaying the algorithm over the maps.
- **A depth cap is mandatory.** `kDepthCap = 3` in `GuideBuilder.cpp`. The graph is small
  today — the deepest chain is `iron_scythe` → `iron_ingot` → `iron_ore`, raw at depth 2 — but
  a cyclic or deep recipe set must never become an infinite loop on an agent's turn.

### 5.3 Reuse

`FormatRecipe` and `FormatSeconds` were extracted from the anonymous namespace in
`src/GameECS.cpp` into **`include/RecipeFormatting.h`** (now `inline`, behaviour unchanged).
They moved because the guide had to describe a recipe from `AgentActions.h`, which cannot see a
file-local namespace. `GameECS.cpp` includes the new header, and `FormatStationRecipes` still
calls the same `FormatRecipe` — so a station and a guide cannot describe one recipe differently.

That function's own comment fixes the item-naming convention:

> One line per recipe, written with content ids rather than display names, because the id is
> the exact token `[CRAFT ...]` and the recipe files use.

So the derived skeleton uses **content ids** (`iron_ingot`), because those are the tokens the
agent must actually emit when it crafts. Display names remain the convention for *trade and
inventory* (`ItemStack`'s comment: "Trade terms are written the way `[INVENTORY]` prints
them"), and the guide deliberately does not mix the two (decision 8). The one exception is the
`@ <station>` clause, which names the station by its `DisplayName` — the same way a station
introduces itself when the agent arrives.

---

## 6. Output shape

Terse and structured, not prose. This is the **actual output of the algorithm replayed over the
shipped content and `maps/Map_Village.json`**, so it is what you should see in-game there (the
`iron_ingot` and `iron_vein` hints are the demonstration hints added with the feature).

**`[GUIDE blacksmithing]`** — the whole chain, roots in sorted recipe-id order:

```
Blacksmithing - 2 recipe(s).
- iron_ingot (1 iron_ore -> iron_ingot) takes 3s @ Smeltery - Gives 5 blacksmithing XP
  hint: Smelt in batches when you can: the walk between the vein and the smeltery is the slow part, not the fire.
  - iron_ore: raw, from Iron Vein (harvestable)
    hint: The vein will not follow you home: work it where it stands, and take only what you can carry.
- iron_scythe (3 iron_ingot -> iron_scythe) takes 8s @ Anvil - Gives 8 blacksmithing XP
  - iron_ingot (1 iron_ore -> iron_ingot) takes 3s @ Smeltery - Gives 5 blacksmithing XP
    hint: Smelt in batches when you can: the walk between the vein and the smeltery is the slow part, not the fire.
    - iron_ore: raw, from Iron Vein (harvestable)
      hint: The vein will not follow you home: work it where it stands, and take only what you can carry.
```

**`[GUIDE farming]`** — an activity with no recipe at all. This is decisions 15 and 16 doing
the work: it is the only place an agent can learn that a wheat field advances farming, and that
it needs a scythe in hand before setting out.

```
Trained by harvesting:
- Apple Tree (harvestable) - Gives 3 farming XP
- Mature Wheat (harvestable) - Requires equipped: scythe - Gives 2 farming XP
```

**`[INFO wheat]`** — the terminal case, one hop, read from the world:

```
wheat: raw, from Mature Wheat (harvestable) - Requires equipped: scythe - Gives 2 farming XP
```

**`[INFO iron_scythe]`** — a crafted item, one hop, no recursion:

```
iron_scythe (3 iron_ingot -> iron_scythe) takes 8s @ Anvil - Gives 8 blacksmithing XP
```

**These answers are world-dependent by design**, and that is the point of deriving them. The
example above comes from a map that actually contains the anvil, the smeltery and an iron vein
— `maps/Map_Village.json` or `maps/Map_Smithy.json`. The chain and the vocabulary are
content-wide, but the `@ <station>` clause and the raw-source line report only what is placed
in the loaded map. On a map without them the same command names the same recipes with no
station, and `iron_ore` reads `raw; no source found in this world`. Note the default map is
`maps/Map_Farm.json`, which places a single chest, so a fresh run shows exactly that until a
smithy map is loaded: the guide is honest about an empty world instead of inventing stations
that are not there.

The one-hop answer names its inputs, which is precisely what the model should ask about
next — the drill-down is self-teaching.

Brevity is a correctness concern, not an aesthetic one — see §9 on history cost.

---

## 7. The authored hint layer

The skeleton is generated; hints are authored. A hint exists for exactly one purpose: to
state a fact the data model does not hold.

**Two storage locations**, because there are two kinds of subject:

| subject | where the hint goes | example |
|---|---|---|
| something **crafted** | `"hint"` in `data/recipes/<id>.json` | `iron_ingot.json` |
| something **gathered** | `"hint"` in `data/objects/<name>.json` | `iron_vein.json` |

A raw material has no recipe, so the recipe file cannot carry its hint; the object that yields
it is the thing the fact is about. `ObjectFactory::ApplyTemplate` reads the optional top-level
key into a `Hint` component, removed and re-set on every template application so a re-applied
template (Evolvable growth, a placed item becoming a world object) cannot keep a stale hint.

```json
{
  "name": "Iron Ingot",
  "inputs": [{ "item": "iron_ore", "count": 1 }],
  "outputs": [{ "item": "iron_ingot", "chance": 1.0 }],
  "craftTime": 3.0,
  "trains": { "blacksmithing": 5 },
  "hint": "Smelt in batches when you can: the walk between the vein and the smeltery is the slow part, not the fire."
}
```

```json
{
  "name": "Iron Vein",
  "components": ["Interactable", "Harvestable", "Obstacle"],
  "drops": [{ "item": "iron_ore", "chance": 1.0 }],
  "hint": "The vein will not follow you home: work it where it stands, and take only what you can carry."
}
```

Note the object hint needs **no** entry in the `"components"` array: it is read directly by
`ApplyTemplate`, the same way `"description"` becomes `ObjectDescription`.

**Rules for authors:**

1. **Never restate a number the registry already knows.** Writing "needs 1 iron ore" in a
   hint duplicates the derived line and creates the drift this design exists to avoid.
2. **State the unmodelled.** Ordering wisdom, prerequisites, failure modes, tone — anything
   with no component behind it.
3. **A hint is rendered after its derived line**, so a stale hint looks obviously redundant
   next to a correct generated line rather than silently replacing it.

This mirrors the split the codebase already uses for skills: `SkillDef::description` is
flavour that is *"never a mechanical claim"*, because the mechanical clause is generated from
the registered links. The guide applies the same principle at the recipe and object level.

Per-recipe and per-object are the **only** hint locations (decision 7). A topic-level overview
file for prose that spans recipes — an introduction, an ordering rule — is a plausible later
addition, but such prose has no single subject to attach to, so it is deferred rather than
designed.

---

## 8. Argument resolution and discovery

Two verbs, one namespace each — so there is no resolution order and nothing can be
mis-resolved. Matching is case-insensitive, with the same tolerance `ResolveStationRecipe`
already shows in accepting `"Flour"` for `flour`.

| invocation | resolves against | on failure |
|---|---|---|
| `[GUIDE $TOPIC]` | `SkillRegistry` ids | list the valid topics |
| `[INFO $ITEM]` | item ids: recipe outputs ∪ harvestable drops | list the valid items |
| `[GUIDE]` | — | list the valid topics |
| `[INFO]` | — | list the valid items |

**Nothing is listed in the spawn prompt.** That would reintroduce exactly the prompt bloat
these commands exist to avoid, and it is unnecessary: the bare forms and the failure paths
enumerate on demand. This is the pattern `InvalidAction::getSuccessMessage()` already uses for
the command vocabulary, so the valid set is learned the first time it is needed and pays no
rent in every request.

**Registration order is load-bearing.** Both verbs are registered with the other knowledge
verbs in `Game::ECSInit`, *before* the hidden `[GENERIC_INTERACT]` catch-all. That catch-all
matches `[A-Z_]+` and would otherwise swallow both. `AgentBrain::ParseMessageCommand` scans the
registry in registration order and returns the first `regex_search` hit, so position decides
which verb claims the text.

---

## 9. Cost, and why output stays terse

`NPCContext::history` is **never trimmed** and is replayed in full on every request. Any text
a guide returns is therefore permanent for the life of that NPC, not just for the current
turn. Three consequences:

- Answers must be **short and structured**. A hundred words of friendly prose per guide is a
  permanent tax on every subsequent request from that agent.
- The model may ask repeatedly as the answer scrolls out of its attention. Each call is a
  full LLM round trip, so spam is a real cost even though the command is free.
- If this becomes a problem the correct fix is **history compaction**, not special-casing
  these commands — trimming is a general gap that affects every long-lived NPC.

Derivation helps here: a generated chain is naturally terse, where authored prose is not.

The command split is the primary lever. `[INFO]` exists precisely so a model that needs one
step does not pay for the whole tree, and because it drills down one hop at a time, the context
it accumulates is proportional to what it actually needed — rather than to the size of the
topic it started from.

---

## 10. What was built

| file | change |
|---|---|
| `include/RecipeFormatting.h` | **new.** `FormatSeconds` + `FormatRecipe`, extracted from `GameECS.cpp` unchanged apart from `inline`. |
| `include/GuideBuilder.h` | **new.** `Topics`, `ItemIds`, `Resolve`, `BuildTopicGuide`, `BuildItemInfo`. |
| `src/GuideBuilder.cpp` | **new.** The derivation in §5.2. No AI, no rendering, no mutation. |
| `include/Components.h` | `Hint` component. Deliberately not reflected — it is set from template JSON by `ObjectFactory`, like `ObjectDescription`. |
| `src/ObjectFactory.cpp` | `ApplyTemplate` reads the optional top-level `"hint"` into `Hint`. Its `Requires` setter now calls the extracted `ParseRequiredTags` — same warnings, no behaviour change. |
| `include/RequiresParsing.h` | **new.** `ParseRequiredTags`, extracted from the `Requires` setter so the spawner and the guide share one parser. Header-only: a pure function over JSON with no ECS dependency. |
| `include/RecipeRegistry.h` | `CraftRecipe::hint`. |
| `src/RecipeRegistry.cpp` | Parses `"hint"`. Never fails the load: a wrong hint is a wording problem, not a broken recipe. |
| `include/TrainingParsing.h` | **reused, unmodified.** A template's `"trains"` block is parsed with the same `ParseTrainingGrants` the recipe and object loaders use, so the guide cannot disagree with the engine about what a block means. |
| `include/AgentActions.h` | `GuideAction`, `ItemInfoAction`, and a `FormatNameList` helper. |
| `src/GameECS.cpp` | Removed the two extracted formatters; registered both commands before `LoadMap` and before `[GENERIC_INTERACT]`. |
| `CMakeLists.txt` | `src/GuideBuilder.cpp` added to `SOURCES` — without this it would silently never compile. |
| `data/recipes/iron_ingot.json` | Demonstration recipe hint. |
| `data/objects/iron_vein.json` | Demonstration raw-source hint. |

Not touched: `AgentBrain`, the `AIRequest` systems, the providers, the threading, and the
progression skill system. These commands are synchronous, self-contained queries over content
data, so they need **no runtime AI verification** — they can be exercised by a model turn, or
read statically.

### Known limitations

- **A recipe offered by several stations reports only the first** found in world order. That is
  advisory colour rather than an exhaustive answer; making it exhaustive would mean listing every
  station, which is more tokens for little gain.
- **A multi-producer input expands only the first producer** in the chain walk, so the tree does
  not fan out. `[INFO <input>]` lists all of them, which is where alternatives belong.
- **`[INFO]` lists every item type in the content**, not only ones reachable now (see Q2).
- **A station is named only when one exists in the world**, so `[GUIDE]` on a map without an
  anvil silently omits the `@ Anvil` clause, while an absent raw source is stated explicitly.
  The asymmetry is deliberate — one is derived from content, the other from presence — but it
  is worth watching (see Q2).
- **A recipe that is both a topic root and a dependency is expanded twice.** `[GUIDE cooking]`
  lists `flour` as a root and then again beneath `bread`, because each root gets its own
  subtree. Redundant, though it costs little at four recipes. Collapsing a re-encountered
  recipe to a one-line "shown above" reference would fix it (see Q6).

---

## 11. Risks

| risk | mitigation |
|---|---|
| **Removes exploration.** A perfect walkthrough turns improvisation into script-following, and convergent blacksmiths. | No locations, no route (decisions 9, 10, 12). Revisit if agents stop exploring. |
| **History bloat.** Untrimmed `ctx.history` makes every answer permanent. | Terse structured output; `[INFO]` for the narrow case; compact history if it bites. |
| **Spam.** Repeated calls are repeated LLM round trips. | `[GUIDE]` answers the whole chain in one call, so drilling is a choice rather than a necessity. |
| **Hint drift.** Hints are hand-written and can describe removed mechanics. | Hints never restate derivable numbers (rule 1). |
| **`[INFO]` mistaken for `[EXAMINE_ITEM]`.** Both are "about an item". | The registered rule states the distinction in its first clause: type versus held instance. |
| **`[INFO]` mistaken for `[CRAFT]`.** One is read-only, the other spends ingredients. | Rule prose states it plainly; both new commands are read-only, so a misfire costs a turn, not materials. |
| **Silent under-delivery.** A one-hop `[INFO]` answer may leave a model feeling unblocked while a deeper chain exists. | The one-hop line names its inputs, which is exactly what to ask about next; `[GUIDE]` exists for the wide view. |
| **Over-coupling to skills.** Topics are skill ids, so deleting a skill silently deletes a topic. | Acceptable: the topic set is exactly the activity set. Note it in `SkillRegistry`'s docs. |
| **XP becomes an optimisable reward.** Decisions 15/16 hand the agent numbers it previously could not see, and agents optimise what they can measure — a blacksmith may grind the most efficient subject instead of playing a role. | **Owner's call: test it empirically first, change nothing pre-emptively.** If grind behaviour does appear in transcripts, drop the amount and report only *that* a subject trains a skill and which one — the knowledge still helps, the gradient disappears. |

---

## 12. Open questions

- **Q1. Non-crafting activities, still half-open.** Decision 15 means `[GUIDE farming]` now
  names what advances it — Apple Tree and Mature Wheat — but not the *production* chain around
  it: `wheat_seed` → `Evolvable` stages → `wheat_mature` → harvest. That lives in
  `Seed`/`Evolvable`/`Harvestable` rather than in `RecipeRegistry`. Should the guide learn to
  walk it, so a farming guide also explains planting and growth, or is "what trains it" the
  right scope?
- **Q2. Content versus presence.** The vocabulary and the chain are content-wide, but the
  station clause and the raw-source line are world-driven. That means an absent station is
  silent while an absent source is stated. Should both report presence, both stay silent, or
  is the current split right — and should `[INFO]` mark an item whose source is not in this
  world at all?
- **Q3. Depth cap value.** `kDepthCap = 3` covers today's four-recipe graph. Is there a
  principled limit, or is an arbitrary constant fine until content demands more?
- **Q4. Player access.** Decision 13 keeps this agent-only for the village milestone. The
  player path would need its own window or command, and the standing decision is that the two
  paths stay separate for now.
- **Q5. Item-definition hints.** An item that is neither crafted nor dropped (an equippable
  with no source in the world) has nowhere to carry a hint. `data/items/*.json` could hold one.
  No such item exists yet, so this is unexercised.
- **Q6. Repeated subtree in a topic guide.** Should a recipe already expanded at this depth be
  collapsed to a reference instead of re-expanded? It would shorten `[GUIDE cooking]`, at the
  cost of a second output form to read.
