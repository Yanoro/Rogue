# NPC templates and brains

Two content registries, and the line between them.

## What problem this solves

Before this, a character's starting configuration lived only in a map's `npcs[]`
entry (`NPCData`): stats, skills, gear, race, look, background, goals. Anything
shared by two blacksmiths had to be written twice, and a character that should
carry the same starting sheet in two maps had no name to carry it by. There was
already an art-only registry (`ActorSpriteRegistry` — "art for a being, and
nothing about being one"); this is the layer above it.

## The two registries

`data/npcs/<id>.json` is a **template**: everything a character starts with.

- `name`, `description`
- `sprite` (an `ActorSpriteRegistry` id), `portrait`, `race`
- `slots` (body override, as in a map)
- `stats` (by stat id, over the registry baselines)
- `skills` (stage/level, as in a map)
- `inventory`, `equipped` (object template keys, as in a map)
- `brain` (optional)

`data/brains/<id>.json` is **what drives it**:

- `name`, `description`
- `modelProfile` (an `ai.profiles` id)
- `background` (the `%BACKGROUND%` text)
- `goals.terminal` (seeded read-only into the TaskList, as in a map)

A template with no brain is a body with no agent. That is the non-LLM case, and it is
why the two join by reference instead of the registry being split into "NPCs" and
"creatures": whether something is LLM-driven is a property of its *control*, not of its
body. The same wolf body can be an enemy in one map and a companion in another by
changing one field.

## A template is a seed, not a class

A "Blacksmith" template says where a character **starts**: the smith's strength and
craft. It does not say where the character goes, and nothing enforces it. `createNPC`
copies the values onto the entity (`GiveDefaultStats`, `ApplyStartingSkills`,
`ApplyStartingStats`, `GiveBody`, equipment, `TaskList`), and the instance owns them
afterwards. A blacksmith can stop smithing, sell the hammer and take up farming; the
template is unchanged.

This is also why goals live on the brain and not on the template: what motivates an
agent is context, and context is the brain's job. A template may carry a `background`,
but a brain's context outranks it.

## Resolution at spawn

A map entry may name a template and a brain, and override any field:

```json
{ "template": "blacksmith", "brain": "townsfolk", "name": "Bram",
  "position": [12, 40], "stats": { "strength": 15 } }
```

Precedence, implemented in `Game::ResolveNpcTemplate` before `createNPC`:

1. **map** — whatever the entry says wins,
2. **brain** — context: background (over the template's), goals,
3. **template** — everything the map left empty.

A reference to an unknown template, brain, race, actor sprite or portrait is reported at
load and again at spawn, and the character is still created from whatever remains.

## What this is not

- **No species layer.** Races still supply body slots and art; a wolf template that
  wants a wolf body would need one. That is deliberately deferred: implementing a
  species, its parts and its skills for every creature is not worth it yet.
- **No inheritance.** Templates are flat; duplication across archetypes is accepted for
  now, and composing them (base + overrides) is a later layer.
- **No per-NPC model backend.** A brain's `modelProfile` is validated against
  `ai.profiles` and reported, but a run still has one agent backend. Backends are
  shared because conversation context is keyed per instance; per-NPC routing is a
  separate change.
- **No behaviour system.** A brain drives an `AgentBrain`; a brainless template has no
  behaviour at all. Pack hunting, aggro and patrol are C++ components and systems that
  do not exist yet — data cannot invent mechanics.

## Content editor

Both registries are content kinds (`npcs`, `brains`), so the Content Editor lists and
edits them with the same generic form as tiles, objects and actor sprites. Saving writes
an override under `content.editor`, which `Game::LoadContentOverrides` applies at
startup.
